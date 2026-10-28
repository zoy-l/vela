#include "compiler/aot/function_emitter.h"
#include "compiler/aot/native_roots.h"
#include "compiler/aot/native_values.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/aot/native_image_emitter.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "runtime/exception.h"
#include "runtime/native_abi.h"

namespace vela::compiler::aot {
namespace {

std::size_t TemporaryArgumentCount(const ir::Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::CallIndirect:
      return static_cast<std::size_t>(instruction.index) + 2;
    case Opcode::CallInterface:
      return static_cast<std::size_t>(instruction.immediate);
    case Opcode::BuiltinCall:
      return BuiltinArity(static_cast<Builtin>(instruction.index));
    case Opcode::AsyncCall:
      return AsyncArity(static_cast<AsyncOperation>(instruction.index));
    case Opcode::StringInterpolate:
      return instruction.rhs;
    case Opcode::StringConcat:
    case Opcode::EqualString:
    case Opcode::NotEqualString:
    case Opcode::EqualValue:
    case Opcode::NotEqualValue:
      return 2;
    case Opcode::SetField:
    case Opcode::SetIndex:
    case Opcode::Print:
    case Opcode::SetGlobal:
    case Opcode::NewOk:
    case Opcode::NewErr:
    case Opcode::NewFuture:
    case Opcode::IsType:
    case Opcode::CheckedCast:
    case Opcode::ExceptionValue:
    case Opcode::Throw:
    case Opcode::Rethrow:
      return 1;
    default:
      return 0;
  }
}

}  // namespace

llvm::Value* FunctionEmitter::Load(ir::RegisterId reg) {
  return builder_.CreateLoad(ValueType(module_.getContext(), source_.value_types.at(reg)),
                             slots_.at(reg));
}

llvm::Value* FunctionEmitter::Payload(ir::RegisterId reg) {
  auto* value = Load(reg);
  return HasValidity(source_.value_types.at(reg)) ? builder_.CreateExtractValue(value, 1) : value;
}

llvm::Value* FunctionEmitter::Reference(ir::RegisterId reg) {
  return DecodeValue(builder_, Encode(reg), {.kind = Kind::Class, .name = {}});
}

void FunctionEmitter::Store(ir::RegisterId reg, llvm::Value* value, const SourceType& type) {
  builder_.CreateStore(Convert(builder_, value, type, source_.value_types.at(reg)), slots_.at(reg));
}

void FunctionEmitter::Finish(llvm::Value* status) {
  status_->addIncoming(status, builder_.GetInsertBlock());
  builder_.CreateBr(cleanup_);
}

void FunctionEmitter::SetPosition() {
  builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(instruction_index_)),
                       instruction_slot_);
}

llvm::Value* FunctionEmitter::SourcePosition() {
  static_assert(std::has_unique_object_representations_v<runtime::TracePosition>);
  const runtime::TracePosition position{
      .function = static_cast<std::uint32_t>(function_index_),
      .instruction = static_cast<std::uint32_t>(instruction_index_),
  };
  const auto bytes =
      std::bit_cast<std::array<std::uint8_t, sizeof(runtime::TracePosition)>>(position);
  auto* value = llvm::ConstantDataArray::get(module_.getContext(), bytes);
  // 位置不可变且由产物持有；不用动态 alloca，避免冷分支阻止包装层内联。
  auto* global = new llvm::GlobalVariable(module_, value->getType(), true,
                                          llvm::GlobalValue::PrivateLinkage, value,
                                          "source.position");
  global->setAlignment(llvm::Align(alignof(runtime::TracePosition)));
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  return global;
}

void FunctionEmitter::Fail(llvm::Value* status) {
  if (leaf_entry_) {
    auto failure =
        module_.getOrInsertFunction("vela_native_v1_leaf_failure", builder_.getInt8Ty(),
                                    builder_.getPtrTy(), builder_.getPtrTy(), builder_.getInt8Ty());
    Finish(builder_.CreateCall(failure, {body_->getArg(0), SourcePosition(), status}));
    return;
  }
  SetPosition();
  if (!source_.instructions.empty() &&
      !ir::AnalyzeEffects(source_.instructions.at(instruction_index_)).MayCollectOnSuccess()) {
    // 只有错误封套可能分配时，将保根写入也放到失败分支；成功路径上的借用无需登记。
    StoreRoots(instruction_index_);
  }
  auto failure = module_.getOrInsertFunction("vela_native_v1_failure", builder_.getInt8Ty(),
                                             builder_.getPtrTy(), builder_.getInt8Ty());
  auto* propagated = builder_.CreateCall(failure, {body_->getArg(0), status});
  const auto handler = source_.instructions.empty()
                           ? std::optional<std::uint32_t>()
                           : source_.instructions.at(instruction_index_).handler;
  if (handler) {
    auto pending = module_.getOrInsertFunction("vela_native_v1_has_exception", builder_.getInt8Ty(),
                                               builder_.getPtrTy());
    auto* has_exception = builder_.CreateCall(pending, {body_->getArg(0)});
    auto* unhandled = llvm::BasicBlock::Create(module_.getContext(), "unhandled", body_);
    // handler 的下一安全点会登记其活动值；此处到 handler 之间不再分配。
    for (std::size_t slot = 0; slot < roots_.count; ++slot) {
      ClearRoot(slot);
    }
    const auto target = source_.handlers.at(*handler).target;
    builder_.CreateCondBr(builder_.CreateICmpNE(has_exception, builder_.getInt8(0)),
                          blocks_.at(target), unhandled);
    builder_.SetInsertPoint(unhandled);
  }
  if (source_.is_async) {
    EmitAsyncFailure(propagated);
  } else {
    Finish(propagated);
  }
}

void FunctionEmitter::Check(llvm::Value* condition, ExecutionStatus failure) {
  auto* failed = llvm::BasicBlock::Create(module_.getContext(), "failed", body_);
  auto* next = llvm::BasicBlock::Create(module_.getContext(), "checked", body_);
  builder_.CreateCondBr(condition, next, failed,
                        llvm::MDBuilder(module_.getContext()).createLikelyBranchWeights());
  builder_.SetInsertPoint(failed);
  Fail(Status(builder_, failure));
  builder_.SetInsertPoint(next);
}

void FunctionEmitter::CheckStatus(llvm::Value* status) {
  auto* failed = llvm::BasicBlock::Create(module_.getContext(), "call.failed", body_);
  auto* next = llvm::BasicBlock::Create(module_.getContext(), "call.returned", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(status, Status(builder_, ExecutionStatus::Returned)),
                        next, failed,
                        llvm::MDBuilder(module_.getContext()).createLikelyBranchWeights());
  builder_.SetInsertPoint(failed);
  Fail(status);
  builder_.SetInsertPoint(next);
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
        condition = builder_.CreateNot(IsPresent(builder_, Load(instruction.lhs), type));
      }
      const auto position = static_cast<std::size_t>(&instruction - source_.instructions.data());
      auto* next = blocks_.at(position + 1);
      ClearRoots(position);
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
      if (!callee.native_symbol.empty()) {
        arguments.push_back(imports_.at(instruction.index));
      }
      // 各类结果都可放入对齐的单个 ABI 暂存槽；循环和多调用不增加帧大小。
      arguments.push_back(result_);
      auto* status =
          builder_.CreateCall(generated_.subspan(instruction.index).front().body, arguments);
      CheckStatus(status);
      if (callee.return_type.kind == Kind::Void) {
        Store(instruction.dst, builder_.getFalse(), callee.return_type);
      } else {
        Store(instruction.dst,
              builder_.CreateLoad(ValueType(module_.getContext(), callee.return_type), result_),
              callee.return_type);
      }
      break;
    }
    case Opcode::Await:
      EmitAwait(instruction);
      break;
    case Opcode::NewFuture:
    case Opcode::SleepFuture:
    case Opcode::AsyncCall:
      return EmitAsyncInstruction(instruction);
    case Opcode::Return: {
      if (source_.is_async) {
        EmitAsyncReturn(instruction);
        break;
      }
      if (source_.return_type.kind != Kind::Void) {
        auto* result = Convert(builder_, Load(instruction.dst),
                               source_.value_types.at(instruction.dst), source_.return_type);
        builder_.CreateStore(result, Output());
      }
      Finish(Status(builder_, ExecutionStatus::Returned));
      break;
    }
    default:
      return EmitReference(instruction);
  }
  return {};
}

std::expected<void, std::string> FunctionEmitter::PrepareFrame() {
  auto& context = module_.getContext();
  if (source_.value_types.size() != source_.register_count) {
    return std::unexpected("AOT requires upstream virtual value types");
  }
  slots_.resize(source_.register_count);
  std::size_t scratch_count = roots_.count == 0 && !source_.is_async ? 0 : 1;
  for (const auto& saved : roots_.suspended) {
    if (!saved.empty()) {
      scratch_count = std::max(scratch_count, saved.size() + 1);
    }
  }
  std::vector<llvm::Type*> fields;
  fields.reserve(slots_.size() + 2);
  for (std::size_t index = 0; index < slots_.size(); ++index) {
    const auto& type = source_.value_types.at(index);
    fields.push_back(type.kind == Kind::Invalid ? builder_.getInt8Ty() : ValueType(context, type));
  }
  for (const auto& instruction : source_.instructions) {
    if ((instruction.opcode == Opcode::GetIndex || instruction.opcode == Opcode::SetIndex ||
         (instruction.opcode == Opcode::BuiltinCall &&
          instruction.index == static_cast<std::uint32_t>(Builtin::ArrayPush))) &&
        instruction.array_type == ir::kInvalidArrayType) {
      return std::unexpected("AOT requires upstream array storage types");
    }
    scratch_count = std::max(scratch_count, TemporaryArgumentCount(instruction));
    const auto access = ir::AnalyzeRegisters(instruction, program_.functions);
    bool has_type =
        !access.defines_value || source_.value_types.at(instruction.dst).kind != Kind::Invalid;
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
      has_type = has_type && source_.value_types.at(reg).kind != Kind::Invalid;
    });
    if (!has_type) {
      return std::unexpected(
          "AOT function " + source_.name + " is missing an upstream value type for opcode " +
          std::to_string(static_cast<unsigned>(instruction.opcode)) + " (index " +
          std::to_string(instruction.index) + ", result " + std::to_string(instruction.dst) + ")");
    }
  }
  fields.push_back(llvm::ArrayType::get(NativeRecordType(context), scratch_count));
  fields.push_back(NativeRecordType(context));
  frame_type_ = llvm::StructType::get(context, fields);
  storage_bytes_ = module_.getDataLayout().getTypeAllocSize(frame_type_);
  frame_bytes_ = storage_bytes_ + roots_.count * sizeof(runtime::Value);
  if (frame_bytes_ <= kMaximumNativeStackBytes) {
    // 局部值的地址不传给 helper；独立 alloca 可提升为 SSA。将它们与 ABI 暂存区
    // 放在同一结构中会让暂存区的地址逃逸阻止提升，导致纯算术循环持续读写栈。
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      if (source_.value_types.at(index).kind != Kind::Invalid) {
        slots_.at(index) = builder_.CreateAlloca(fields.at(index));
      }
    }
    arguments_ = builder_.CreateAlloca(fields.at(slots_.size()));
    result_ = builder_.CreateAlloca(fields.back());
  }
  return {};
}

bool FunctionEmitter::CanUseLeafEntry() const {
  if (source_.is_async || !source_.native_symbol.empty() || !source_.handlers.empty() ||
      roots_.count != 0 || frame_bytes_ > kMaximumNativeStackBytes ||
      !IsScalar(source_.return_type)) {
    return false;
  }
  if (!std::ranges::all_of(source_.value_types, [](const SourceType& type) {
        return type.kind == Kind::Invalid || IsScalar(type);
      })) {
    return false;
  }
  return std::ranges::all_of(source_.instructions, [](const ir::Instruction& instruction) {
    // Ready 全局只读标量槽；惰性初始化分支在 helper 内临时登记调用者逻辑帧。
    // 标量 return 不装箱；其余操作依赖共享效果信息，禁止调用、分配或观察状态。
    if (instruction.opcode == Opcode::Return || instruction.opcode == Opcode::GetGlobal) {
      return true;
    }
    const auto effects = ir::AnalyzeEffects(instruction);
    return !effects.MayCollectOnSuccess() && !effects.reads_state && !effects.writes_state;
  });
}

void FunctionEmitter::BindFrameStorage() {
  if (frame_bytes_ <= kMaximumNativeStackBytes) {
    return;
  }
  for (std::size_t index = 0; index < slots_.size(); ++index) {
    if (source_.value_types.at(index).kind != Kind::Invalid) {
      slots_.at(index) =
          builder_.CreateStructGEP(frame_type_, frame_storage_, static_cast<unsigned>(index));
    }
  }
  const auto scratch = static_cast<unsigned>(slots_.size());
  arguments_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch);
  result_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 1);
}

void FunctionEmitter::EmitImport() {
  auto* context = body_->getArg(0);
  auto* function = builder_.getInt32(static_cast<std::uint32_t>(function_index_));
  std::vector<llvm::Value*> arguments{context, function};
  std::vector<llvm::Type*> types{builder_.getPtrTy(), builder_.getInt32Ty()};
  for (std::size_t index = 0; index < source_.parameter_count; ++index) {
    const auto reg = static_cast<ir::RegisterId>(index);
    if (const auto slot = roots_.slots.at(reg)) {
      WriteHeapValue(RootAddress(*slot), reg);
    }
    auto* value = Load(reg);
    if (source_.value_types.at(reg).kind == Kind::Bool) {
      value = builder_.CreateZExt(value, builder_.getInt8Ty());
    }
    arguments.push_back(value);
    types.push_back(value->getType());
  }
  const auto& return_type = source_.return_type;
  auto* result_type = return_type.kind == Kind::Bool || return_type.kind == Kind::Void
                          ? builder_.getInt8Ty()
                          : ValueType(module_.getContext(), return_type);
  // ABI 标量结果直接写入调用者槽，避免 import 包装层再复制一次。
  arguments.push_back(Output());
  types.push_back(builder_.getPtrTy());
  auto* thunk = body_->getArg(static_cast<unsigned>(source_.parameter_count + 1));
  auto* signature = llvm::FunctionType::get(builder_.getInt8Ty(), types, false);
  CheckStatus(builder_.CreateCall(signature, thunk, arguments));
  if (return_type.kind != Kind::Void) {
    llvm::Value* value = builder_.CreateLoad(result_type, Output());
    if (return_type.kind == Kind::Bool) {
      value = builder_.CreateICmpNE(value, builder_.getInt8(0));
    }
    builder_.CreateStore(value, Output());
  }
  Finish(Status(builder_, ExecutionStatus::Returned));
}

std::expected<void, std::string> FunctionEmitter::Emit() {
  auto& context = module_.getContext();
  auto* entry = llvm::BasicBlock::Create(context, "entry", body_);
  builder_.SetInsertPoint(entry);
  const auto prepared = PrepareFrame();
  if (!prepared) {
    return prepared;
  }
  leaf_entry_ = CanUseLeafEntry();
  EmitFrameEntry();
  BindFrameStorage();
  CacheImports();
  if (kind_ == NativeEntryKind::Initial) {
    for (std::size_t index = 0; index < source_.parameter_count; ++index) {
      builder_.CreateStore(body_->getArg(static_cast<unsigned>(index + 1)), slots_.at(index));
    }
  }
  if (!source_.native_symbol.empty()) {
    EmitImport();
    return {};
  }
  for (std::size_t index = 0; index <= source_.instructions.size(); ++index) {
    blocks_.push_back(llvm::BasicBlock::Create(context, "instruction", body_));
  }
  if (kind_ == NativeEntryKind::Resume) {
    EmitResumeDispatch();
  } else {
    builder_.CreateBr(blocks_.front());
  }
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    const auto& instruction = source_.instructions.at(index);
    instruction_index_ = index;
    builder_.SetInsertPoint(blocks_.at(index));
    const auto effects = ir::AnalyzeEffects(instruction);
    if (!leaf_entry_ && effects.MayCollectOnSuccess()) {
      // 调用者位置必须在进入被调用方前可见；throw 和异步 helper 也可能直接捕获栈。
      // 纯检查在失败块内记录位置，成功的算术循环不执行位置写入。
      SetPosition();
      StoreRoots(index);
    }
    auto emitted = EmitInstruction(instruction);
    if (!emitted) {
      return emitted;
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      ClearRoots(index);
      builder_.CreateBr(blocks_.at(index + 1));
    }
  }
  builder_.SetInsertPoint(blocks_.back());
  Finish(Status(builder_, ExecutionStatus::InvalidEntry));
  return {};
}

std::expected<NativeFunctionMetrics, std::string> EmitFunction(
    llvm::Module& module,
    const ir::Program& program,
    std::size_t function_index,
    std::span<const GeneratedFunction> generated) {
  const auto roots = PlanNativeRoots(program.functions.at(function_index), program.functions);
  FunctionEmitter initial(module, program, function_index, generated, roots,
                          NativeEntryKind::Initial);
  if (const auto emitted = initial.Emit(); !emitted) {
    return std::unexpected(emitted.error());
  }
  NativeFunctionMetrics metrics{.shadow_slots = roots.count, .frame_bytes = initial.frame_bytes()};
  for (const auto& stores : roots.stores) {
    metrics.root_stores += stores.size();
  }
  if (generated.subspan(function_index).front().resume != nullptr) {
    FunctionEmitter resume(module, program, function_index, generated, roots,
                           NativeEntryKind::Resume);
    if (const auto emitted = resume.Emit(); !emitted) {
      return std::unexpected(emitted.error());
    }
    metrics.frame_bytes = std::max(metrics.frame_bytes, resume.frame_bytes());
  }
  return metrics;
}

}  // namespace vela::compiler::aot
