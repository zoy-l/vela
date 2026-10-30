#include "compiler/native/function_emitter.h"
#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Alignment.h>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "runtime/native_abi.h"
#include "runtime/native_deoptimization.h"
#include "runtime/native_function.h"
#include "runtime/native_memory_layout.h"
#include "runtime/value.h"

namespace vela::compiler::native {
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
  const auto& type = source_.value_types.at(reg);
  if (IsRecovering()) {
    auto* value = builder_.CreateLoad(NativeRecordType(module_.getContext()), slots_.at(reg));
    return DecodeValue(builder_, value, type);
  }
  return builder_.CreateLoad(ValueType(module_.getContext(), type), slots_.at(reg));
}

llvm::Value* FunctionEmitter::Payload(ir::RegisterId reg) {
  auto* value = Load(reg);
  return HasValidity(source_.value_types.at(reg)) ? builder_.CreateExtractValue(value, 1) : value;
}

llvm::Value* FunctionEmitter::Reference(ir::RegisterId reg) {
  return DecodeValue(builder_, Encode(reg), {.kind = Kind::Class, .name = {}});
}

void FunctionEmitter::Store(ir::RegisterId reg, llvm::Value* value, const SourceType& type) {
  const auto& target = source_.value_types.at(reg);
  auto* converted = Convert(builder_, value, type, target);
  builder_.CreateStore(IsRecovering() ? EncodeValue(builder_, converted, target) : converted,
                       slots_.at(reg));
}

void FunctionEmitter::Finish(llvm::Value* status) {
  status_->addIncoming(status, builder_.GetInsertBlock());
  builder_.CreateBr(cleanup_);
}

void FunctionEmitter::SetPosition() {
  builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(instruction_index_)),
                       instruction_slot_);
}

void FunctionEmitter::Poll() {
  auto poll = module_.getOrInsertFunction("vela_native_v3_safepoint", builder_.getInt8Ty(),
                                          builder_.getPtrTy());
  CheckStatus(builder_.CreateCall(poll, {body_->getArg(0)}));
  if (tier_ == NativeCodeTier::Optimized) {
    RecoverIfInvalidated();
  }
}

void FunctionEmitter::Fail(llvm::Value* status) {
  AbortConstructions();
  if (leaf_entry_) {
    const auto& live = roots_.stores.at(instruction_index_);
    for (std::size_t index = 0; index < live.size(); ++index) {
      WriteHeapValue(RootAddress(index), live.at(index));
    }
    auto failure = module_.getOrInsertFunction(
        "vela_native_v2_leaf_failure", builder_.getInt8Ty(), builder_.getPtrTy(),
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getPtrTy(), builder_.getInt64Ty());
    auto* roots = live.empty() ? llvm::ConstantPointerNull::get(builder_.getPtrTy()) : root_slots_;
    const auto position = runtime::EncodeNativeTracePosition({
        .function = static_cast<std::uint32_t>(function_index_),
        .instruction = static_cast<std::uint32_t>(instruction_index_),
    });
    Finish(builder_.CreateCall(failure, {
                                            body_->getArg(0),
                                            builder_.getInt64(position),
                                            status,
                                            roots,
                                            builder_.getInt64(live.size()),
                                        }));
    return;
  }
  SetPosition();
  if (!source_.instructions.empty() &&
      !ir::AnalyzeEffects(source_.instructions.at(instruction_index_)).MayCollectOnSuccess() &&
      roots_.safepoints.at(instruction_index_) == 0) {
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
  if (program_.native_mode == NativeProgramMode::Reloadable) {
    auto* stopped = llvm::BasicBlock::Create(module_.getContext(), "execution.stopped", body_);
    auto* propagate = llvm::BasicBlock::Create(module_.getContext(), "execution.failure", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(status, Status(builder_, ExecutionStatus::ExecutionStopped)), stopped,
        propagate);
    builder_.SetInsertPoint(stopped);
    Finish(status);
    builder_.SetInsertPoint(propagate);
  }
  Fail(status);
  builder_.SetInsertPoint(next);
}

void FunctionEmitter::EmitConstant(const ir::Instruction& instruction) {
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
    default:
      std::unreachable();
  }
}

std::expected<void, std::string> FunctionEmitter::EmitInstruction(
    const ir::Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::LoadInt:
    case Opcode::LoadDouble:
      EmitConstant(instruction);
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
      if (program_.native_mode == NativeProgramMode::Reloadable && callee.native_symbol.empty() &&
          tier_ == NativeCodeTier::Baseline && callee.identity.local == 0 &&
          callee.identity.declaration != DeclarationId{}) {
        for (std::size_t index = 0; index < callee.parameter_count; ++index) {
          const auto reg = static_cast<ir::RegisterId>(instruction.lhs + index);
          const auto& type = callee.value_types.at(index);
          auto* value = Convert(builder_, Load(reg), source_.value_types.at(reg), type);
          Pack(EncodeValue(builder_, value, type), index);
        }
        auto helper = module_.getOrInsertFunction(
            "vela_native_v3_call_declaration", builder_.getInt8Ty(), builder_.getPtrTy(),
            builder_.getInt64Ty(), builder_.getPtrTy(), builder_.getInt64Ty(), builder_.getPtrTy());
        CheckStatus(
            builder_.CreateCall(helper, {
                                            body_->getArg(0),
                                            builder_.getInt64(callee.identity.declaration.value),
                                            arguments_,
                                            builder_.getInt64(callee.parameter_count),
                                            result_,
                                        }));
        Unpack(instruction.dst);
        break;
      }
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
      // Call 的效果要求调用者先登记逻辑帧；内联后仍保留该事实，避免为嵌套宿主调用
      // 复制一条不可能发生的最外层入口清理路径。
      auto* depth = builder_.CreateLoad(
          builder_.getInt64Ty(),
          ByteAddress(body_->getArg(0), runtime::NativeMemoryLayout::kExecutionDepth));
      builder_.CreateAssumption(builder_.CreateICmpNE(depth, builder_.getInt64(0)));
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

std::expected<std::size_t, std::string> FunctionEmitter::TemporaryCount() const {
  std::size_t scratch_count = roots_.count == 0 && !source_.is_async ? 0 : 1;
  for (const auto& saved : roots_.suspended) {
    if (!saved.empty()) {
      scratch_count = std::max(scratch_count, saved.size() + 1);
    }
  }
  for (const auto& instruction : source_.instructions) {
    if ((instruction.opcode == Opcode::GetIndex || instruction.opcode == Opcode::SetIndex ||
         (instruction.opcode == Opcode::BuiltinCall &&
          instruction.index == static_cast<std::uint32_t>(Builtin::ArrayPush))) &&
        instruction.array_type == ir::kInvalidArrayType) {
      return std::unexpected("native code requires upstream array storage types");
    }
    scratch_count = std::max(scratch_count, TemporaryArgumentCount(instruction));
    if (program_.native_mode == NativeProgramMode::Reloadable &&
        instruction.opcode == Opcode::Call) {
      scratch_count = std::max(
          scratch_count,
          static_cast<std::size_t>(program_.functions.at(instruction.index).parameter_count));
    }
    const auto access = ir::AnalyzeRegisters(instruction, program_.functions);
    bool has_type =
        !access.defines_value || source_.value_types.at(instruction.dst).kind != Kind::Invalid;
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
      has_type = has_type && source_.value_types.at(reg).kind != Kind::Invalid;
    });
    if (!has_type) {
      return std::unexpected(
          "native function " + source_.name + " is missing an upstream value type for opcode " +
          std::to_string(static_cast<unsigned>(instruction.opcode)) + " (index " +
          std::to_string(instruction.index) + ", result " + std::to_string(instruction.dst) + ")");
    }
  }
  return scratch_count;
}

std::expected<void, std::string> FunctionEmitter::PrepareFrame() {
  auto& context = module_.getContext();
  if (source_.value_types.size() != source_.register_count) {
    return std::unexpected("native code requires upstream virtual value types");
  }
  slots_.resize(source_.register_count);
  if (std::ranges::any_of(source_.instructions, [](const auto& instruction) {
        return instruction.construction.has_value();
      })) {
    construction_depth_ = builder_.CreateAlloca(builder_.getInt32Ty());
    builder_.CreateStore(builder_.getInt32(0), construction_depth_);
  }
  const auto scratch_count = TemporaryCount();
  if (!scratch_count) {
    return std::unexpected(scratch_count.error());
  }
  std::vector<llvm::Type*> fields;
  constexpr std::size_t kAuxiliaryFields = 5;
  fields.reserve(slots_.size() + kAuxiliaryFields);
  for (const auto& type : source_.value_types) {
    fields.push_back(type.kind == Kind::Invalid ? builder_.getInt8Ty() : ValueType(context, type));
  }
  fields.push_back(llvm::ArrayType::get(NativeRecordType(context), *scratch_count));
  fields.push_back(NativeRecordType(context));
  if (tier_ == NativeCodeTier::Optimized) {
    auto* recovery_storage =
        llvm::StructType::get(context, {fields.at(slots_.size()), fields.at(slots_.size() + 1)});
    fields.push_back(llvm::ArrayType::get(builder_.getInt8Ty(), sizeof(runtime::NativeCheckpoint)));
    fields.push_back(llvm::ArrayType::get(NativeRecordType(context), source_.register_count));
    fields.push_back(recovery_storage);
  }
  frame_type_ =
      IsRecovering()
          ? llvm::StructType::get(context, {fields.at(slots_.size()), fields.at(slots_.size() + 1)})
          : llvm::StructType::get(context, fields);
  storage_bytes_ = module_.getDataLayout().getTypeAllocSize(frame_type_);
  frame_bytes_ = storage_bytes_ + (roots_.count * sizeof(runtime::Value));
  if (!IsRecovering() && frame_bytes_ <= kMaximumNativeStackBytes) {
    // 局部值的地址不传给 helper；独立 alloca 可提升为 SSA。将它们与 ABI 暂存区
    // 放在同一结构中会让暂存区的地址逃逸阻止提升，导致纯算术循环持续读写栈。
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      if (source_.value_types.at(index).kind != Kind::Invalid) {
        slots_.at(index) = builder_.CreateAlloca(fields.at(index));
      }
    }
    arguments_ = builder_.CreateAlloca(fields.at(slots_.size()));
    result_ = builder_.CreateAlloca(fields.at(slots_.size() + 1));
    if (tier_ == NativeCodeTier::Optimized) {
      auto* checkpoint = builder_.CreateAlloca(fields.at(slots_.size() + 2));
      checkpoint->setAlignment(llvm::Align(alignof(runtime::NativeCheckpoint)));
      checkpoint_ = checkpoint;
      checkpoint_values_ = builder_.CreateAlloca(fields.at(slots_.size() + 3));
      recovery_storage_ = builder_.CreateAlloca(fields.at(slots_.size() + 4));
    }
  }
  return {};
}

bool FunctionEmitter::CanUseLeafEntry() const {
  if (program_.native_mode == NativeProgramMode::Reloadable || source_.is_async ||
      !source_.native_symbol.empty() || !source_.handlers.empty() ||
      frame_bytes_ > kMaximumNativeStackBytes || !IsScalar(source_.return_type)) {
    return false;
  }
  return std::ranges::all_of(source_.instructions, [&](const ir::Instruction& instruction) {
    // 标量 return 不装箱；字段和数组读写可保留副作用，但不能进入用户代码或分配。
    if (instruction.opcode == Opcode::Return) {
      return true;
    }
    if (instruction.opcode == Opcode::GetGlobal) {
      // 惰性初始化器可能 GC；仅在没有活动引用时沿用延迟逻辑帧入口。
      return roots_.count == 0;
    }
    const auto effects = ir::AnalyzeEffects(instruction);
    return !effects.MayCollectOnSuccess();
  });
}

void FunctionEmitter::BindFrameStorage() {
  if (IsRecovering()) {
    frame_storage_ = builder_.CreateLoad(
        builder_.getPtrTy(),
        ByteAddress(body_->getArg(1), offsetof(runtime::NativeCheckpoint, storage)));
    root_slots_ = builder_.CreateLoad(
        builder_.getPtrTy(),
        ByteAddress(body_->getArg(1), offsetof(runtime::NativeCheckpoint, roots)));
    auto* values = builder_.CreateLoad(
        builder_.getPtrTy(),
        ByteAddress(body_->getArg(1), offsetof(runtime::NativeCheckpoint, values)));
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      slots_.at(index) = ByteAddress(values, index * sizeof(runtime::NativeValue));
    }
    arguments_ = builder_.CreateStructGEP(frame_type_, frame_storage_, 0);
    result_ = builder_.CreateStructGEP(frame_type_, frame_storage_, 1);
    return;
  }
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
  if (tier_ == NativeCodeTier::Optimized) {
    checkpoint_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 2);
    checkpoint_values_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 3);
    recovery_storage_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 4);
  }
}

void FunctionEmitter::EmitImport() {
  using AotCallKind = runtime::NativeFunction::AotCallKind;
  using runtime::NativeMemoryLayout;
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
  const auto host_boundary = [&](const char* symbol) {
    if (program_.native_mode == NativeProgramMode::Reloadable) {
      auto helper = module_.getOrInsertFunction(symbol, builder_.getVoidTy(), builder_.getPtrTy());
      builder_.CreateCall(helper, {context});
    }
  };
  if (program_.native_mode == NativeProgramMode::Reloadable) {
    Poll();
  }
  const auto& return_type = source_.return_type;
  auto* result_type = return_type.kind == Kind::Bool || return_type.kind == Kind::Void
                          ? builder_.getInt8Ty()
                          : ValueType(module_.getContext(), return_type);
  auto* entries = body_->getArg(static_cast<unsigned>(source_.parameter_count + 1));
  auto* target = builder_.CreateExtractValue(entries, 0);
  auto* call_kind = builder_.CreateExtractValue(entries, 1);
  const auto emit_direct = [&](AotCallKind kind) {
    std::vector<llvm::Value*> direct_arguments(arguments.begin() + 2, arguments.end());
    std::vector<llvm::Type*> direct_types(types.begin() + 2, types.end());
    if (kind == AotCallKind::DirectWithContext) {
      direct_arguments.insert(direct_arguments.begin(),
                              ByteAddress(context, NativeMemoryLayout::kExecutionContext));
      direct_types.insert(direct_types.begin(), builder_.getPtrTy());
    }
    auto* direct_result = return_type.kind == Kind::Void ? builder_.getVoidTy() : result_type;
    auto* signature = llvm::FunctionType::get(direct_result, direct_types, false);
    host_boundary("vela_native_v3_host_enter");
    auto* value = builder_.CreateCall(signature, target, direct_arguments);
    host_boundary("vela_native_v3_host_exit");
    if (return_type.kind != Kind::Void) {
      llvm::Value* result = value;
      if (return_type.kind == Kind::Bool) {
        result = builder_.CreateICmpNE(result, builder_.getInt8(0));
      }
      builder_.CreateStore(result, Output());
    }
    Finish(Status(builder_, ExecutionStatus::Returned));
  };
  if (return_type.kind == Kind::Void || return_type.kind == Kind::Bool ||
      return_type.kind == Kind::Int || return_type.kind == Kind::Double) {
    auto* direct = llvm::BasicBlock::Create(module_.getContext(), "import.direct", body_);
    auto* checked = llvm::BasicBlock::Create(module_.getContext(), "import.checked", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpNE(call_kind,
                              builder_.getInt8(static_cast<std::uint8_t>(AotCallKind::Checked))),
        direct, checked);
    builder_.SetInsertPoint(direct);
    auto* with_context = llvm::BasicBlock::Create(module_.getContext(), "import.context", body_);
    auto* plain = llvm::BasicBlock::Create(module_.getContext(), "import.plain", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(
            call_kind, builder_.getInt8(static_cast<std::uint8_t>(AotCallKind::DirectWithContext))),
        with_context, plain);
    builder_.SetInsertPoint(with_context);
    emit_direct(AotCallKind::DirectWithContext);
    builder_.SetInsertPoint(plain);
    emit_direct(AotCallKind::Direct);
    builder_.SetInsertPoint(checked);
  }
  arguments.push_back(Output());
  types.push_back(builder_.getPtrTy());
  auto* signature = llvm::FunctionType::get(builder_.getInt8Ty(), types, false);
  host_boundary("vela_native_v3_host_enter");
  auto* called = builder_.CreateCall(signature, target, arguments);
  host_boundary("vela_native_v3_host_exit");
  CheckStatus(called);
  if (return_type.kind != Kind::Void) {
    llvm::Value* value = builder_.CreateLoad(result_type, Output());
    if (return_type.kind == Kind::Bool) {
      value = builder_.CreateICmpNE(value, builder_.getInt8(0));
    }
    builder_.CreateStore(value, Output());
  }
  Finish(Status(builder_, ExecutionStatus::Returned));
}

std::expected<void, std::string> FunctionEmitter::EmitInstructionBlock(std::size_t index) {
  const auto& instruction = source_.instructions.at(index);
  instruction_index_ = index;
  builder_.SetInsertPoint(blocks_.at(index));
  const auto effects = ir::AnalyzeEffects(instruction);
  const auto safepoint = roots_.safepoints.at(index) != 0;
  if (!leaf_entry_ && (effects.MayCollectOnSuccess() || safepoint)) {
    // 调用者位置在进入被调用方前可见；纯检查仅在失败分支登记，算术成功路径不写位置。
    SetPosition();
    StoreRoots(index);
    StoreCheckpoint();
  }
  if (safepoint) {
    Poll();
  }
  if (instruction.construction) {
    BeginConstruction();
  }
  if (checkpoint_ != nullptr && effects.MayCollectOnSuccess()) {
    builder_.CreateStore(
        builder_.getInt8(static_cast<std::uint8_t>(runtime::NativeCheckpointPhase::AwaitingResult)),
        ByteAddress(checkpoint_, offsetof(runtime::NativeCheckpoint, phase)));
  }
  const auto emitted = EmitInstruction(instruction);
  if (!emitted) {
    return emitted;
  }
  if (instruction.completes_construction) {
    EndConstruction();
  }
  if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
    ClearRoots(index);
    builder_.CreateBr(blocks_.at(index + 1));
  }
  return {};
}

std::expected<void, std::string> FunctionEmitter::Emit() {
  auto& context = module_.getContext();
  auto* entry = llvm::BasicBlock::Create(context, "entry", body_);
  builder_.SetInsertPoint(entry);
  GuardResume();
  const auto prepared = PrepareFrame();
  if (!prepared) {
    return prepared;
  }
  leaf_entry_ = CanUseLeafEntry();
  EmitFrameEntry();
  BindFrameStorage();
  InitializeCheckpoint();
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
  if (IsRecovering()) {
    EmitRecoveryDispatch();
  } else if (kind_ == NativeEntryKind::Resume) {
    EmitResumeDispatch();
  } else {
    builder_.CreateBr(blocks_.front());
  }
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    if (const auto emitted = EmitInstructionBlock(index); !emitted) {
      return emitted;
    }
  }
  builder_.SetInsertPoint(blocks_.back());
  Finish(Status(builder_, ExecutionStatus::InvalidEntry));
  return {};
}

std::expected<NativeFunctionMetrics, std::string> EmitFunction(
    llvm::Module& module,
    const ir::Program& program,
    llvm::Constant* image,
    std::size_t function_index,
    std::span<const GeneratedFunction> generated,
    NativeDebugging debugging,
    NativeCodeTier tier) {
  const auto roots = PlanNativeRoots(program.functions.at(function_index), program.functions,
                                     program.native_mode, debugging);
  FunctionEmitter initial(module, program, image, function_index, generated, roots,
                          NativeEntryKind::Initial, tier);
  if (const auto emitted = initial.Emit(); !emitted) {
    return std::unexpected(emitted.error());
  }
  NativeFunctionMetrics metrics{.shadow_slots = roots.count, .frame_bytes = initial.frame_bytes()};
  for (const auto& stores : roots.stores) {
    metrics.root_stores += stores.size();
  }
  if (generated.subspan(function_index).front().resume != nullptr) {
    FunctionEmitter resume(module, program, image, function_index, generated, roots,
                           NativeEntryKind::Resume, tier);
    if (const auto emitted = resume.Emit(); !emitted) {
      return std::unexpected(emitted.error());
    }
    metrics.frame_bytes = std::max(metrics.frame_bytes, resume.frame_bytes());
  }
  const auto& function = generated.subspan(function_index).front();
  if (tier == NativeCodeTier::Baseline && function.recovery != nullptr) {
    FunctionEmitter recovery(module, program, image, function_index, generated, roots,
                             NativeEntryKind::RecoverInitial, tier);
    if (const auto emitted = recovery.Emit(); !emitted) {
      return std::unexpected(emitted.error());
    }
    if (function.resume_recovery != nullptr) {
      FunctionEmitter resume(module, program, image, function_index, generated, roots,
                             NativeEntryKind::RecoverResume, tier);
      if (const auto emitted = resume.Emit(); !emitted) {
        return std::unexpected(emitted.error());
      }
    }
  }
  return metrics;
}

}  // namespace vela::compiler::native
