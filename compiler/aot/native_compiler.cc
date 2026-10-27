#include "compiler/aot/native_compiler.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "runtime/native_abi.h"

namespace vela::compiler::aot {
namespace {

using SourceType = frontend::TypeDescriptor;
using Kind = SourceType::Kind;
using Builder = llvm::IRBuilder<>;

constexpr std::size_t kMaximumNativeStackBytes = 4096;
constexpr unsigned kIntegerShiftMask = 63;

bool IsScalar(const SourceType& type) {
  return type.kind == Kind::Int || type.kind == Kind::Enum || type.kind == Kind::Double ||
         type.kind == Kind::Bool || type.kind == Kind::Null || type.kind == Kind::Void;
}

llvm::Type* PayloadType(llvm::LLVMContext& context, Kind kind) {
  if (kind == Kind::Double) {
    return llvm::Type::getDoubleTy(context);
  }
  return kind == Kind::Bool || kind == Kind::Null || kind == Kind::Void
             ? llvm::Type::getInt1Ty(context)
             : llvm::Type::getInt64Ty(context);
}

llvm::Type* ValueType(llvm::LLVMContext& context, const SourceType& type) {
  auto* payload = PayloadType(context, type.kind);
  return type.nullable ? llvm::StructType::get(llvm::Type::getInt1Ty(context), payload) : payload;
}

llvm::ConstantInt* Status(Builder& builder, ExecutionStatus status) {
  return builder.getInt8(static_cast<std::uint8_t>(status));
}

std::uint8_t NativeKind(Kind kind) {
  using runtime::NativeValueKind;
  switch (kind) {
    case Kind::Bool:
      return static_cast<std::uint8_t>(NativeValueKind::Boolean);
    case Kind::Int:
    case Kind::Enum:
      return static_cast<std::uint8_t>(NativeValueKind::Integer);
    case Kind::Double:
      return static_cast<std::uint8_t>(NativeValueKind::Double);
    default:
      return static_cast<std::uint8_t>(NativeValueKind::Null);
  }
}

llvm::Value* Convert(Builder& builder,
                     llvm::Value* value,
                     const SourceType& source,
                     const SourceType& target) {
  auto* target_type = ValueType(builder.getContext(), target);
  if (source.kind == Kind::Null || source.kind == Kind::Void) {
    return llvm::Constant::getNullValue(target_type);
  }
  auto* payload = source.nullable ? builder.CreateExtractValue(value, 1) : value;
  if (!target.nullable) {
    return payload;
  }
  auto* present = source.nullable ? builder.CreateExtractValue(value, 0) : builder.getTrue();
  auto* result = builder.CreateInsertValue(llvm::PoisonValue::get(target_type), present, 0);
  return builder.CreateInsertValue(result, payload, 1);
}

struct Generated final {
  llvm::Function* body = nullptr;
  llvm::Function* entry = nullptr;
};

class FunctionEmitter final {
 public:
  FunctionEmitter(llvm::Module& module,
                  const ir::Program& program,
                  std::size_t function_index,
                  std::span<const Generated> generated)
      : module_(module),
        program_(program),
        source_(program.functions.at(function_index)),
        generated_(generated),
        body_(generated.subspan(function_index).front().body),
        builder_(module.getContext()) {}

  std::expected<void, std::string> Emit();

 private:
  llvm::Value* Load(ir::RegisterId reg);
  llvm::Value* Payload(ir::RegisterId reg);
  void Store(ir::RegisterId reg, llvm::Value* value, const SourceType& type);
  void Check(llvm::Value* condition, ExecutionStatus failure);
  void CheckStatus(llvm::Value* status);
  void Finish(llvm::Value* status);
  void EmitInteger(const ir::Instruction& instruction);
  void EmitDouble(const ir::Instruction& instruction);
  void EmitComparison(const ir::Instruction& instruction);
  void EmitValueEquality(const ir::Instruction& instruction);
  std::expected<void, std::string> EmitInstruction(const ir::Instruction& instruction);

  llvm::Module& module_;
  const ir::Program& program_;
  const ir::Function& source_;
  std::span<const Generated> generated_;
  llvm::Function* body_;
  Builder builder_;
  std::vector<llvm::AllocaInst*> slots_;
  std::vector<llvm::BasicBlock*> blocks_;
  llvm::BasicBlock* cleanup_ = nullptr;
  llvm::PHINode* status_ = nullptr;
};

llvm::Value* FunctionEmitter::Load(ir::RegisterId reg) {
  return builder_.CreateLoad(ValueType(module_.getContext(), source_.value_types.at(reg)),
                             slots_.at(reg));
}

llvm::Value* FunctionEmitter::Payload(ir::RegisterId reg) {
  auto* value = Load(reg);
  return source_.value_types.at(reg).nullable ? builder_.CreateExtractValue(value, 1) : value;
}

void FunctionEmitter::Store(ir::RegisterId reg, llvm::Value* value, const SourceType& type) {
  builder_.CreateStore(Convert(builder_, value, type, source_.value_types.at(reg)), slots_.at(reg));
}

void FunctionEmitter::Finish(llvm::Value* status) {
  status_->addIncoming(status, builder_.GetInsertBlock());
  builder_.CreateBr(cleanup_);
}

void FunctionEmitter::Check(llvm::Value* condition, ExecutionStatus failure) {
  auto* failed = llvm::BasicBlock::Create(module_.getContext(), "failed", body_);
  auto* next = llvm::BasicBlock::Create(module_.getContext(), "checked", body_);
  builder_.CreateCondBr(condition, next, failed);
  builder_.SetInsertPoint(failed);
  Finish(Status(builder_, failure));
  builder_.SetInsertPoint(next);
}

void FunctionEmitter::CheckStatus(llvm::Value* status) {
  auto* failed = llvm::BasicBlock::Create(module_.getContext(), "call.failed", body_);
  auto* next = llvm::BasicBlock::Create(module_.getContext(), "call.returned", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(status, Status(builder_, ExecutionStatus::Returned)),
                        next, failed);
  builder_.SetInsertPoint(failed);
  Finish(status);
  builder_.SetInsertPoint(next);
}

void FunctionEmitter::EmitInteger(const ir::Instruction& instruction) {
  auto* left = Payload(instruction.lhs);
  llvm::Value* result = nullptr;
  const auto opcode = instruction.opcode;
  if (opcode == Opcode::NegInt) {
    Check(builder_.CreateICmpNE(left, builder_.getInt64(std::bit_cast<std::uint64_t>(
                                          std::numeric_limits<std::int64_t>::min()))),
          ExecutionStatus::IntegerOverflow);
    result = builder_.CreateNeg(left);
  } else if (opcode == Opcode::BitNotInt) {
    result = builder_.CreateNot(left);
  } else {
    auto* right = Payload(instruction.rhs);
    switch (opcode) {
      case Opcode::AddInt:
      case Opcode::SubInt:
      case Opcode::MulInt: {
        auto intrinsic = llvm::Intrinsic::sadd_with_overflow;
        if (opcode == Opcode::SubInt) {
          intrinsic = llvm::Intrinsic::ssub_with_overflow;
        } else if (opcode == Opcode::MulInt) {
          intrinsic = llvm::Intrinsic::smul_with_overflow;
        }
        auto* checked = builder_.CreateBinaryIntrinsic(intrinsic, left, right);
        Check(builder_.CreateNot(builder_.CreateExtractValue(checked, 1)),
              ExecutionStatus::IntegerOverflow);
        result = builder_.CreateExtractValue(checked, 0);
        break;
      }
      case Opcode::DivInt:
      case Opcode::ModInt: {
        Check(builder_.CreateICmpNE(right, builder_.getInt64(0)), ExecutionStatus::DivisionByZero);
        auto* minimum = builder_.CreateICmpEQ(left, builder_.getInt64(std::bit_cast<std::uint64_t>(
                                                        std::numeric_limits<std::int64_t>::min())));
        auto* negative_one = builder_.CreateICmpEQ(
            right, builder_.getInt64(std::numeric_limits<std::uint64_t>::max()));
        Check(builder_.CreateNot(builder_.CreateAnd(minimum, negative_one)),
              ExecutionStatus::IntegerOverflow);
        result = opcode == Opcode::DivInt ? builder_.CreateSDiv(left, right)
                                          : builder_.CreateSRem(left, right);
        break;
      }
      case Opcode::BitAndInt:
        result = builder_.CreateAnd(left, right);
        break;
      case Opcode::BitOrInt:
        result = builder_.CreateOr(left, right);
        break;
      case Opcode::BitXorInt:
        result = builder_.CreateXor(left, right);
        break;
      case Opcode::ShiftLeftInt:
        result = builder_.CreateShl(left, builder_.CreateAnd(right, kIntegerShiftMask));
        break;
      case Opcode::ShiftRightInt:
        result = builder_.CreateAShr(left, builder_.CreateAnd(right, kIntegerShiftMask));
        break;
      case Opcode::ShiftUnsignedRightInt:
        result = builder_.CreateLShr(left, builder_.CreateAnd(right, kIntegerShiftMask));
        break;
      default:
        llvm_unreachable("integer opcode dispatch must be exhaustive");
    }
  }
  Store(instruction.dst, result, {.kind = Kind::Int, .name = "int"});
}

void FunctionEmitter::EmitDouble(const ir::Instruction& instruction) {
  auto* left = Payload(instruction.lhs);
  llvm::Value* result = nullptr;
  if (instruction.opcode == Opcode::NegDouble) {
    result = builder_.CreateFNeg(left);
  } else {
    auto* right = Payload(instruction.rhs);
    switch (instruction.opcode) {
      case Opcode::AddDouble:
        result = builder_.CreateFAdd(left, right);
        break;
      case Opcode::SubDouble:
        result = builder_.CreateFSub(left, right);
        break;
      case Opcode::MulDouble:
        result = builder_.CreateFMul(left, right);
        break;
      case Opcode::DivDouble:
        Check(builder_.CreateFCmpUNE(right, llvm::ConstantFP::get(right->getType(), 0)),
              ExecutionStatus::DivisionByZero);
        result = builder_.CreateFDiv(left, right);
        break;
      default:
        llvm_unreachable("double opcode dispatch must be exhaustive");
    }
  }
  Store(instruction.dst, result, {.kind = Kind::Double, .name = "double"});
}

void FunctionEmitter::EmitComparison(const ir::Instruction& instruction) {
  auto* left = Payload(instruction.lhs);
  auto* right = Payload(instruction.rhs);
  llvm::Value* result = nullptr;
  switch (instruction.opcode) {
    case Opcode::EqualInt:
    case Opcode::EqualBool:
      result = builder_.CreateICmpEQ(left, right);
      break;
    case Opcode::NotEqualInt:
    case Opcode::NotEqualBool:
      result = builder_.CreateICmpNE(left, right);
      break;
    case Opcode::LessInt:
      result = builder_.CreateICmpSLT(left, right);
      break;
    case Opcode::LessEqualInt:
      result = builder_.CreateICmpSLE(left, right);
      break;
    case Opcode::GreaterInt:
      result = builder_.CreateICmpSGT(left, right);
      break;
    case Opcode::GreaterEqualInt:
      result = builder_.CreateICmpSGE(left, right);
      break;
    case Opcode::EqualDouble:
      result = builder_.CreateFCmpOEQ(left, right);
      break;
    case Opcode::NotEqualDouble:
      result = builder_.CreateFCmpUNE(left, right);
      break;
    case Opcode::LessDouble:
      result = builder_.CreateFCmpOLT(left, right);
      break;
    case Opcode::LessEqualDouble:
      result = builder_.CreateFCmpOLE(left, right);
      break;
    case Opcode::GreaterDouble:
      result = builder_.CreateFCmpOGT(left, right);
      break;
    case Opcode::GreaterEqualDouble:
      result = builder_.CreateFCmpOGE(left, right);
      break;
    default:
      llvm_unreachable("comparison opcode dispatch must be exhaustive");
  }
  Store(instruction.dst, result, {.kind = Kind::Bool, .name = "bool"});
}

void FunctionEmitter::EmitValueEquality(const ir::Instruction& instruction) {
  const auto& left_type = source_.value_types.at(instruction.lhs);
  const auto& right_type = source_.value_types.at(instruction.rhs);
  auto* left = Load(instruction.lhs);
  auto* right = Load(instruction.rhs);
  auto* left_present = left_type.nullable ? builder_.CreateExtractValue(left, 0)
                                          : builder_.getInt1(left_type.kind != Kind::Null);
  auto* right_present = right_type.nullable ? builder_.CreateExtractValue(right, 0)
                                            : builder_.getInt1(right_type.kind != Kind::Null);
  llvm::Value* payload_equal = builder_.getFalse();
  if (left_type.kind == right_type.kind && left_type.kind != Kind::Null) {
    auto* left_payload = left_type.nullable ? builder_.CreateExtractValue(left, 1) : left;
    auto* right_payload = right_type.nullable ? builder_.CreateExtractValue(right, 1) : right;
    payload_equal = left_type.kind == Kind::Double
                        ? builder_.CreateFCmpOEQ(left_payload, right_payload)
                        : builder_.CreateICmpEQ(left_payload, right_payload);
  }
  auto* both_null = builder_.CreateNot(builder_.CreateOr(left_present, right_present));
  auto* both_present = builder_.CreateAnd(left_present, right_present);
  auto* equal = builder_.CreateOr(both_null, builder_.CreateAnd(both_present, payload_equal));
  auto* result = instruction.opcode == Opcode::EqualValue ? equal : builder_.CreateNot(equal);
  Store(instruction.dst, result, {.kind = Kind::Bool, .name = "bool"});
}

std::expected<void, std::string> FunctionEmitter::EmitInstruction(
    const ir::Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::LoadNull:
      Store(instruction.dst, builder_.getFalse(), {.kind = Kind::Null, .name = "null"});
      break;
    case Opcode::LoadBool:
      Store(instruction.dst, builder_.getInt1(instruction.immediate != 0),
            {.kind = Kind::Bool, .name = "bool"});
      break;
    case Opcode::LoadInt:
      Store(instruction.dst, builder_.getInt64(std::bit_cast<std::uint64_t>(instruction.immediate)),
            {.kind = Kind::Int, .name = "int"});
      break;
    case Opcode::LoadDouble:
      Store(instruction.dst,
            llvm::ConstantFP::get(builder_.getDoubleTy(), source_.doubles.at(instruction.index)),
            {.kind = Kind::Double, .name = "double"});
      break;
    case Opcode::Move:
      Store(instruction.dst, Load(instruction.lhs), source_.value_types.at(instruction.lhs));
      break;
    case Opcode::AddInt:
    case Opcode::SubInt:
    case Opcode::MulInt:
    case Opcode::DivInt:
    case Opcode::ModInt:
    case Opcode::NegInt:
    case Opcode::BitNotInt:
    case Opcode::BitAndInt:
    case Opcode::BitOrInt:
    case Opcode::BitXorInt:
    case Opcode::ShiftLeftInt:
    case Opcode::ShiftRightInt:
    case Opcode::ShiftUnsignedRightInt:
      EmitInteger(instruction);
      break;
    case Opcode::AddDouble:
    case Opcode::SubDouble:
    case Opcode::MulDouble:
    case Opcode::DivDouble:
    case Opcode::NegDouble:
      EmitDouble(instruction);
      break;
    case Opcode::EqualInt:
    case Opcode::NotEqualInt:
    case Opcode::LessInt:
    case Opcode::LessEqualInt:
    case Opcode::GreaterInt:
    case Opcode::GreaterEqualInt:
    case Opcode::EqualBool:
    case Opcode::NotEqualBool:
    case Opcode::EqualDouble:
    case Opcode::NotEqualDouble:
    case Opcode::LessDouble:
    case Opcode::LessEqualDouble:
    case Opcode::GreaterDouble:
    case Opcode::GreaterEqualDouble:
      EmitComparison(instruction);
      break;
    case Opcode::EqualValue:
    case Opcode::NotEqualValue:
      EmitValueEquality(instruction);
      break;
    case Opcode::AndBool:
    case Opcode::OrBool: {
      auto* left = Payload(instruction.lhs);
      auto* right = Payload(instruction.rhs);
      auto* result = instruction.opcode == Opcode::AndBool ? builder_.CreateAnd(left, right)
                                                           : builder_.CreateOr(left, right);
      Store(instruction.dst, result, {.kind = Kind::Bool, .name = "bool"});
      break;
    }
    case Opcode::NotBool:
      Store(instruction.dst, builder_.CreateNot(Payload(instruction.lhs)),
            {.kind = Kind::Bool, .name = "bool"});
      break;
    case Opcode::Jump:
      builder_.CreateBr(blocks_.at(instruction.index));
      break;
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull: {
      llvm::Value* condition = nullptr;
      if (instruction.opcode == Opcode::JumpIfFalse) {
        condition = builder_.CreateNot(Payload(instruction.lhs));
      } else {
        const auto& type = source_.value_types.at(instruction.lhs);
        condition = type.nullable
                        ? builder_.CreateNot(builder_.CreateExtractValue(Load(instruction.lhs), 0))
                        : builder_.getInt1(type.kind == Kind::Null);
      }
      const auto position = static_cast<std::size_t>(&instruction - source_.instructions.data());
      auto* next = blocks_.at(position + 1);
      builder_.CreateCondBr(condition, blocks_.at(instruction.index), next);
      break;
    }
    case Opcode::Call: {
      const auto& callee = program_.functions.at(instruction.index);
      std::vector<llvm::Value*> arguments{body_->getArg(0)};
      for (std::size_t index = 0; index < callee.parameter_count; ++index) {
        const auto reg = static_cast<ir::RegisterId>(instruction.lhs + index);
        arguments.push_back(Convert(builder_, Load(reg), source_.value_types.at(reg),
                                    callee.value_types.at(index)));
      }
      // 结果槽在入口分配，循环内调用不会持续增长原生栈。
      Builder entry(&body_->getEntryBlock(), body_->getEntryBlock().begin());
      auto* result = entry.CreateAlloca(ValueType(module_.getContext(), callee.return_type));
      arguments.push_back(result);
      auto* status =
          builder_.CreateCall(generated_.subspan(instruction.index).front().body, arguments);
      CheckStatus(status);
      if (callee.return_type.kind == Kind::Void) {
        Store(instruction.dst, builder_.getFalse(), callee.return_type);
      } else {
        Store(instruction.dst,
              builder_.CreateLoad(ValueType(module_.getContext(), callee.return_type), result),
              callee.return_type);
      }
      break;
    }
    case Opcode::Return: {
      if (source_.return_type.kind != Kind::Void) {
        auto* result = Convert(builder_, Load(instruction.dst),
                               source_.value_types.at(instruction.dst), source_.return_type);
        builder_.CreateStore(result, body_->getArg(source_.parameter_count + 1));
      }
      Finish(Status(builder_, ExecutionStatus::Returned));
      break;
    }
    default:
      return std::unexpected("AOT lowering is not implemented for opcode " +
                             std::to_string(static_cast<unsigned>(instruction.opcode)));
  }
  return {};
}

std::expected<void, std::string> FunctionEmitter::Emit() {
  auto& context = module_.getContext();
  if (source_.value_types.size() != source_.register_count) {
    return std::unexpected("AOT requires upstream virtual value types");
  }
  auto* entry = llvm::BasicBlock::Create(context, "entry", body_);
  builder_.SetInsertPoint(entry);
  slots_.resize(source_.register_count);
  std::size_t stack_bytes = 0;
  for (std::size_t index = 0; index < slots_.size(); ++index) {
    const auto& type = source_.value_types.at(index);
    if (type.kind == Kind::Invalid) {
      continue;
    }
    if (!IsScalar(type)) {
      return std::unexpected("AOT reference lowering is not implemented for " + type.name);
    }
    auto* native_type = ValueType(context, type);
    stack_bytes += module_.getDataLayout().getTypeAllocSize(native_type);
    slots_.at(index) = builder_.CreateAlloca(native_type);
  }
  for (const auto& instruction : source_.instructions) {
    if (instruction.opcode == Opcode::Call) {
      const auto& callee = program_.functions.at(instruction.index);
      stack_bytes +=
          module_.getDataLayout().getTypeAllocSize(ValueType(context, callee.return_type));
    }
    const auto access = ir::AnalyzeRegisters(instruction, program_.functions);
    bool has_type = !access.defines_value || slots_.at(instruction.dst) != nullptr;
    ir::ForEachUse(instruction, access,
                   [&](ir::RegisterId reg) { has_type = has_type && slots_.at(reg) != nullptr; });
    if (!has_type) {
      return std::unexpected("AOT instruction is missing an upstream virtual value type");
    }
  }
  if (stack_bytes > kMaximumNativeStackBytes) {
    return std::unexpected("AOT function exceeds the native stack frame limit");
  }
  cleanup_ = llvm::BasicBlock::Create(context, "cleanup", body_);
  Builder cleanup(cleanup_);
  status_ = cleanup.CreatePHI(builder_.getInt8Ty(), 0, "status");
  auto exit = module_.getOrInsertFunction("vela_native_v1_frame_exit", cleanup.getVoidTy(),
                                          cleanup.getPtrTy());
  cleanup.CreateCall(exit, {body_->getArg(0)});
  cleanup.CreateRet(status_);
  auto enter = module_.getOrInsertFunction("vela_native_v1_frame_enter", builder_.getInt8Ty(),
                                           builder_.getPtrTy(), builder_.getInt64Ty());
  auto* entered = builder_.CreateCall(enter, {body_->getArg(0), builder_.getInt64(0)});
  auto* failed = llvm::BasicBlock::Create(context, "enter.failed", body_);
  auto* initialize = llvm::BasicBlock::Create(context, "initialize", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(entered, Status(builder_, ExecutionStatus::Returned)),
                        initialize, failed);
  Builder failure(failed);
  failure.CreateRet(entered);
  builder_.SetInsertPoint(initialize);
  for (std::size_t index = 0; index < source_.parameter_count; ++index) {
    builder_.CreateStore(body_->getArg(static_cast<unsigned>(index + 1)), slots_.at(index));
  }
  for (std::size_t index = 0; index <= source_.instructions.size(); ++index) {
    blocks_.push_back(llvm::BasicBlock::Create(context, "instruction", body_));
  }
  builder_.CreateBr(blocks_.front());
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    const auto& instruction = source_.instructions.at(index);
    if (instruction.handler) {
      return std::unexpected("AOT exception handler lowering is not implemented");
    }
    builder_.SetInsertPoint(blocks_.at(index));
    auto emitted = EmitInstruction(instruction);
    if (!emitted) {
      return emitted;
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      builder_.CreateBr(blocks_.at(index + 1));
    }
  }
  builder_.SetInsertPoint(blocks_.back());
  Finish(Status(builder_, ExecutionStatus::InvalidEntry));
  return {};
}

void EmitWrapper(llvm::Module& module, const ir::Function& source, const Generated& function) {
  auto& context = module.getContext();
  auto* wrapper = function.entry;
  Builder builder(llvm::BasicBlock::Create(context, "entry", wrapper));
  auto* invalid = llvm::BasicBlock::Create(context, "invalid", wrapper);
  auto* unpack = llvm::BasicBlock::Create(context, "unpack", wrapper);
  auto* count_matches =
      builder.CreateICmpEQ(wrapper->getArg(2), builder.getInt64(source.parameter_count));
  auto* has_output = builder.CreateIsNotNull(wrapper->getArg(3));
  auto* valid = builder.CreateAnd(count_matches, has_output);
  if (source.parameter_count != 0) {
    valid = builder.CreateAnd(valid, builder.CreateIsNotNull(wrapper->getArg(1)));
  }
  builder.CreateCondBr(valid, unpack, invalid);
  Builder invalid_builder(invalid);
  invalid_builder.CreateRet(Status(invalid_builder, ExecutionStatus::InvalidEntry));
  builder.SetInsertPoint(unpack);
  std::vector<llvm::Value*> arguments{wrapper->getArg(0)};
  for (std::size_t index = 0; index < source.parameter_count; ++index) {
    const auto& type = source.value_types.at(index);
    auto* address = builder.CreateConstGEP1_64(builder.getInt8Ty(), wrapper->getArg(1),
                                               index * sizeof(runtime::NativeValue));
    auto* kind_address = builder.CreateConstGEP1_64(builder.getInt8Ty(), address,
                                                    offsetof(runtime::NativeValue, kind));
    auto* kind = builder.CreateLoad(builder.getInt8Ty(), kind_address);
    auto* present = builder.CreateICmpEQ(kind, builder.getInt8(NativeKind(type.kind)));
    auto* matches = present;
    if (type.nullable) {
      matches = builder.CreateOr(
          matches, builder.CreateICmpEQ(kind, builder.getInt8(NativeKind(Kind::Null))));
    }
    auto* next = llvm::BasicBlock::Create(context, "argument", wrapper);
    auto* wrong_type = llvm::BasicBlock::Create(context, "type.error", wrapper);
    builder.CreateCondBr(matches, next, wrong_type);
    Builder wrong(wrong_type);
    wrong.CreateRet(Status(wrong, ExecutionStatus::TypeError));
    builder.SetInsertPoint(next);
    auto* bits = builder.CreateLoad(builder.getInt64Ty(), address);
    llvm::Value* payload = bits;
    if (type.kind == Kind::Double) {
      payload = builder.CreateBitCast(bits, builder.getDoubleTy());
    } else if (type.kind == Kind::Bool) {
      auto* canonical = builder.CreateICmpULE(bits, builder.getInt64(1));
      if (type.nullable) {
        canonical = builder.CreateOr(builder.CreateNot(present), canonical);
      }
      auto* checked = llvm::BasicBlock::Create(context, "boolean", wrapper);
      builder.CreateCondBr(canonical, checked, wrong_type);
      builder.SetInsertPoint(checked);
      payload = builder.CreateICmpNE(bits, builder.getInt64(0));
    }
    llvm::Value* argument = payload;
    if (type.nullable) {
      argument =
          builder.CreateInsertValue(llvm::PoisonValue::get(ValueType(context, type)), present, 0);
      argument = builder.CreateInsertValue(argument, payload, 1);
    }
    arguments.push_back(argument);
  }
  auto* result_type = ValueType(context, source.return_type);
  auto* result = builder.CreateAlloca(result_type);
  arguments.push_back(result);
  auto* status = builder.CreateCall(function.body, arguments);
  auto* returned = llvm::BasicBlock::Create(context, "returned", wrapper);
  auto* failed = llvm::BasicBlock::Create(context, "failed", wrapper);
  builder.CreateCondBr(builder.CreateICmpEQ(status, Status(builder, ExecutionStatus::Returned)),
                       returned, failed);
  Builder failure(failed);
  failure.CreateRet(status);
  builder.SetInsertPoint(returned);
  auto* output = wrapper->getArg(3);
  builder.CreateMemSet(output, builder.getInt8(0), sizeof(runtime::NativeValue),
                       llvm::MaybeAlign());
  if (source.return_type.kind != Kind::Void) {
    auto* value = builder.CreateLoad(result_type, result);
    auto* payload = source.return_type.nullable ? builder.CreateExtractValue(value, 1) : value;
    if (source.return_type.kind == Kind::Double) {
      payload = builder.CreateBitCast(payload, builder.getInt64Ty());
    } else if (source.return_type.kind == Kind::Bool) {
      payload = builder.CreateZExt(payload, builder.getInt64Ty());
    }
    builder.CreateStore(payload, output);
    llvm::Value* kind = builder.getInt8(NativeKind(source.return_type.kind));
    if (source.return_type.nullable) {
      kind = builder.CreateSelect(builder.CreateExtractValue(value, 0), kind,
                                  builder.getInt8(NativeKind(Kind::Null)));
    }
    builder.CreateStore(kind, builder.CreateConstGEP1_64(builder.getInt8Ty(), output,
                                                         offsetof(runtime::NativeValue, kind)));
  }
  builder.CreateRet(status);
}

std::expected<std::unique_ptr<llvm::TargetMachine>, std::string> CreateTarget() {
  static const bool initialized = [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    return true;
  }();
  static_cast<void>(initialized);
  const auto triple = llvm::sys::getDefaultTargetTriple();
  std::string error;
  const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (target == nullptr) {
    return std::unexpected(std::move(error));
  }
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> machine(
      target->createTargetMachine(llvm::Triple(triple), "generic", "", options, llvm::Reloc::PIC_));
  if (machine == nullptr) {
    return std::unexpected("unable to create native target machine");
  }
  return machine;
}

void Optimize(llvm::Module& module, llvm::TargetMachine& machine) {
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder passes(&machine);
  passes.registerModuleAnalyses(modules);
  passes.registerCGSCCAnalyses(calls);
  passes.registerFunctionAnalyses(functions);
  passes.registerLoopAnalyses(loops);
  passes.crossRegisterProxies(loops, functions, calls, modules);
  auto pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  pipeline.run(module, modules);
}

frontend::Diagnostics MakeDiagnostics(const ir::Program& program,
                                      std::string message,
                                      const ir::Function* function) {
  frontend::Diagnostic result{.location = {}, .message = std::move(message), .source_path = {}};
  if (function != nullptr) {
    result.location = function->location;
    if (!program.source_paths.empty()) {
      result.source_path = program.source_paths.at(result.location.source_id);
    }
  }
  return {std::move(result)};
}

std::expected<absl::flat_hash_set<std::string>, std::string> SelectExports(
    const ir::Program& program,
    std::span<const std::string> names) {
  absl::flat_hash_map<std::string_view, bool> functions;
  for (const auto& function : program.functions) {
    functions.emplace(function.name, function.is_exported);
  }
  absl::flat_hash_set<std::string> selected;
  for (const auto& name : names) {
    const auto found = functions.find(name);
    if (found == functions.end() || !found->second) {
      return std::unexpected("AOT export must name a public entry-module function: " + name);
    }
    selected.insert(name);
  }
  return selected;
}

}  // namespace

std::expected<NativeArtifact, frontend::Diagnostics> Compile(const ir::Program& program,
                                                             const NativeOptions& options) {
  const auto diagnostic = [&](std::string message, const ir::Function* function = nullptr) {
    return std::unexpected(MakeDiagnostics(program, std::move(message), function));
  };
  auto target = CreateTarget();
  if (!target) {
    return diagnostic(target.error());
  }
  auto& machine = **target;
  const auto selected = SelectExports(program, options.exports);
  if (!selected) {
    return diagnostic(selected.error());
  }
  llvm::LLVMContext context;
  llvm::Module module(options.module_name, context);
  module.setTargetTriple(machine.getTargetTriple());
  module.setDataLayout(machine.createDataLayout());
  std::vector<Generated> generated;
  std::vector<NativeExport> exports;
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    if (function.is_async || !function.native_symbol.empty() || !IsScalar(function.return_type)) {
      return diagnostic("AOT asynchronous, native import or reference lowering is not implemented",
                        &function);
    }
    std::vector<llvm::Type*> parameters{llvm::PointerType::getUnqual(context)};
    for (std::size_t parameter = 0; parameter < function.parameter_count; ++parameter) {
      if (parameter >= function.value_types.size() ||
          !IsScalar(function.value_types.at(parameter))) {
        return diagnostic("AOT requires a supported upstream parameter type", &function);
      }
      parameters.push_back(ValueType(context, function.value_types.at(parameter)));
    }
    parameters.push_back(llvm::PointerType::getUnqual(context));
    auto* body_type = llvm::FunctionType::get(llvm::Type::getInt8Ty(context), parameters, false);
    auto* body =
        llvm::Function::Create(body_type, llvm::GlobalValue::InternalLinkage,
                               options.module_name + "_body_" + std::to_string(index), module);
    auto* pointer = llvm::PointerType::getUnqual(context);
    auto* wrapper_type = llvm::FunctionType::get(
        llvm::Type::getInt8Ty(context),
        {pointer, pointer, llvm::Type::getInt64Ty(context), pointer}, false);
    llvm::Function* entry = nullptr;
    if (selected->contains(function.name)) {
      const auto symbol = options.module_name + "_entry_" + std::to_string(index);
      entry =
          llvm::Function::Create(wrapper_type, llvm::GlobalValue::ExternalLinkage, symbol, module);
      exports.push_back({.name = function.name, .symbol = symbol});
    }
    generated.push_back({.body = body, .entry = entry});
  }
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    auto emitted = FunctionEmitter(module, program, index, generated).Emit();
    if (!emitted) {
      return diagnostic(emitted.error(), &function);
    }
    const auto& native_function = generated.at(index);
    if (native_function.entry != nullptr) {
      EmitWrapper(module, function, native_function);
    }
  }
  std::string verification;
  llvm::raw_string_ostream verifier(verification);
  if (llvm::verifyModule(module, &verifier)) {
    return diagnostic(verification);
  }
  if (options.optimization == NativeOptimization::Release) {
    Optimize(module, machine);
  }
  if (llvm::verifyModule(module, &verifier)) {
    return diagnostic(verification);
  }
  std::string llvm_ir;
  llvm::raw_string_ostream ir_output(llvm_ir);
  module.print(ir_output, nullptr);
  llvm::SmallVector<char, 0> bytes;
  llvm::raw_svector_ostream stream(bytes);
  llvm::legacy::PassManager codegen;
  if (machine.addPassesToEmitFile(codegen, stream, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    return diagnostic("native target cannot emit object files");
  }
  codegen.run(module);
  std::vector<std::byte> object(bytes.size());
  std::ranges::transform(bytes, object.begin(), [](char value) {
    return static_cast<std::byte>(static_cast<unsigned char>(value));
  });
  return NativeArtifact{
      .object = std::move(object),
      .llvm_ir = std::move(llvm_ir),
      .target = machine.getTargetTriple().str(),
      .exports = std::move(exports),
  };
}

}  // namespace vela::compiler::aot
