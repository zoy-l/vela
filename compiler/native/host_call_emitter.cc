#include "compiler/native/function_emitter.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/MDBuilder.h>

#include "base/callable.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_memory_layout.h"

namespace vela::compiler::native {
namespace {

bool HasDirectHostResult(const SourceType& type) {
  return !type.nullable && (type.kind == Kind::Void || type.kind == Kind::Bool ||
                            type.kind == Kind::Int || type.kind == Kind::Double);
}

bool IsHostParameter(const layout::Program& layouts,
                     const CallSignature& signature,
                     const SourceType& type,
                     std::size_t index) {
  if (type.kind != Kind::Class) {
    return false;
  }
  const auto receiver_count = static_cast<std::size_t>(signature.receiver.has_value());
  const auto type_index = signature.receiver && index == 0
                              ? *signature.receiver
                              : signature.parameters[index - receiver_count].type;
  const auto& parameter = layouts.types[type_index];
  return layouts.classes[parameter.index].kind == layout::Class::Kind::Host;
}

}  // namespace

void FunctionEmitter::EmitHostBoundary(HostBorrow change) {
  auto* control = ControlState();
  auto* active = ByteAddress(control, offsetof(runtime::NativeControlState, host_execution));
  if (change == HostBorrow::Enter) {
    if (host_parent_ == nullptr) {
      Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
      host_parent_ = allocation.CreateAlloca(builder_.getPtrTy());
    }
    builder_.CreateStore(builder_.CreateLoad(builder_.getPtrTy(), active), host_parent_);
    builder_.CreateStore(body_->getArg(0), active);
  } else {
    builder_.CreateStore(builder_.CreateLoad(builder_.getPtrTy(), host_parent_), active);
  }
  if (program_.native_mode != NativeProgramMode::Reloadable) {
    return;
  }
  auto* address = ByteAddress(control, offsetof(runtime::NativeControlState, host_borrows));
  auto* depth = builder_.CreateLoad(builder_.getInt64Ty(), address);
  auto* updated = change == HostBorrow::Enter ? builder_.CreateAdd(depth, builder_.getInt64(1))
                                              : builder_.CreateSub(depth, builder_.getInt64(1));
  builder_.CreateStore(updated, address);
}

llvm::Value* FunctionEmitter::EmitHostPointer(llvm::Value* wrapper, const SourceType& type) {
  const auto load = [&] {
    auto* pointer = builder_.CreateLoad(
        builder_.getPtrTy(),
        ByteAddress(wrapper, runtime::NativeMemoryLayout::kObjectNativePayload));
    // 宿主包装在保根期间不移动，原生指针直到包装销毁都不改变，允许循环复用这次借用。
    MarkInvariant(*pointer);
    return pointer;
  };
  if (!type.nullable) {
    return load();
  }
  auto& context = module_.getContext();
  auto* origin = builder_.GetInsertBlock();
  auto* present = llvm::BasicBlock::Create(context, "host.argument.present", body_);
  auto* done = llvm::BasicBlock::Create(context, "host.argument.done", body_);
  builder_.CreateCondBr(builder_.CreateIsNotNull(wrapper), present, done);
  builder_.SetInsertPoint(present);
  auto* pointer = load();
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
  auto* result = builder_.CreatePHI(builder_.getPtrTy(), 2, "host.argument");
  result->addIncoming(llvm::ConstantPointerNull::get(builder_.getPtrTy()), origin);
  result->addIncoming(pointer, present);
  return result;
}

llvm::Value* FunctionEmitter::EmitDirectHostCall(const ir::Function& callee,
                                                 llvm::Value* entries,
                                                 std::span<llvm::Value* const> arguments) {
  auto& context = module_.getContext();
  const auto& return_type = callee.return_type;
  const auto return_kind = return_type.kind;
  auto* result_type = builder_.getVoidTy();
  if (return_kind != Kind::Void) {
    result_type = ValueType(context, return_type);
  }
  auto* target = builder_.CreateExtractValue(entries, 0);
  const auto& call_signature = *callee.signature;
  std::vector<llvm::Value*> parameters(arguments.begin(), arguments.end());
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto& type = callee.value_types[index];
    if (IsHostParameter(program_.layouts, call_signature, type, index)) {
      parameters[index] = EmitHostPointer(parameters[index], type);
    }
  }
  parameters.insert(parameters.begin(),
                    ByteAddress(body_->getArg(0), runtime::NativeMemoryLayout::kExecutionContext));
  std::vector<llvm::Type*> types;
  types.reserve(parameters.size());
  for (auto* value : parameters) {
    types.push_back(value->getType());
  }
  auto* signature = llvm::FunctionType::get(result_type, types, false);
  EmitHostBoundary(HostBorrow::Enter);
  auto* result = builder_.CreateCall(signature, target, parameters);
  // 直接 ABI 与 C++ bool 使用相同的 i1 / zeroext 约定，避免字节 codec 阻断尾调用。
  if (return_kind == Kind::Bool) {
    result->addRetAttr(llvm::Attribute::ZExt);
  }
  for (std::size_t index = 1; index < parameters.size(); ++index) {
    if (parameters[index]->getType()->isIntegerTy(1)) {
      result->addParamAttr(static_cast<unsigned>(index), llvm::Attribute::ZExt);
    }
  }
  EmitHostBoundary(HostBorrow::Exit);
  if (return_kind == Kind::Void) {
    return builder_.getFalse();
  }
  return result;
}

bool FunctionEmitter::CanInlineImportCall(const ir::Function& callee) const {
  if (program_.native_mode != NativeProgramMode::Closed || roots_.debug_values ||
      callee.native_symbol.empty() || !HasDirectHostResult(callee.return_type)) {
    return false;
  }
  // 调用点的根计划包含全部托管实参，包括最后一次使用的 receiver。直接调用
  // 从包装对象固定位置借用原生指针，原生调用期间仍保留调用者的根登记。
  const auto& signature = *callee.signature;
  for (std::size_t index = 0; index < callee.parameter_count; ++index) {
    const auto& type = callee.value_types[index];
    if (IsScalar(type) && !type.nullable) {
      continue;
    }
    if (!IsHostParameter(program_.layouts, signature, type, index)) {
      return false;
    }
  }
  return true;
}

llvm::BasicBlock* FunctionEmitter::EmitInlineImportCall(const ir::Instruction& instruction,
                                                        CallableTarget selected,
                                                        std::span<llvm::Value* const> arguments) {
  const auto& callee = program_.functions.at(selected.function);
  if (!CanInlineImportCall(callee)) {
    return nullptr;
  }
  using AotCallKind = runtime::NativeFunction::AotCallKind;
  using runtime::NativeMemoryLayout;
  using runtime::NativeTraceFrame;
  auto& context = module_.getContext();
  auto* entries = imports_.at(selected.function);
  auto* call_kind = builder_.CreateExtractValue(entries, 1);
  auto* direct = llvm::BasicBlock::Create(context, "call.import.direct", body_);
  auto* checked = llvm::BasicBlock::Create(context, "call.import.checked", body_);
  auto* done = llvm::BasicBlock::Create(context, "call.import.done", body_);
  builder_.CreateCondBr(
      builder_.CreateICmpNE(call_kind,
                            builder_.getInt8(static_cast<std::uint8_t>(AotCallKind::Checked))),
      direct, checked, llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(direct);
  auto* execution = body_->getArg(0);
  auto* depth_address = ByteAddress(execution, NativeMemoryLayout::kExecutionDepth);
  // 调用者逻辑帧已经物化；嵌套调用返回时恢复深度，因此复用本次入口保存的值。
  auto* depth = builder_.CreateAdd(entry_depth_, builder_.getInt64(1));
  Check(HasStackBudget(depth), ExecutionStatus::StackOverflow);
  // 元数据由调用者初始化一次；位置必须逐次重置，因为宿主可修改当前指令位置。
  builder_.CreateStore(builder_.getInt32(selected.function),
                       ByteAddress(import_frame_, offsetof(NativeTraceFrame, position) +
                                                      offsetof(runtime::TracePosition, function)));
  builder_.CreateStore(
      builder_.getInt32(0),
      ByteAddress(import_frame_, offsetof(NativeTraceFrame, position) +
                                     offsetof(runtime::TracePosition, instruction)));
  auto* frame_address = ByteAddress(execution, NativeMemoryLayout::kExecutionFrame);
  builder_.CreateStore(import_frame_, frame_address);
  builder_.CreateStore(builder_.CreateAdd(depth, builder_.getInt64(1)), depth_address);
  auto* result = EmitDirectHostCall(callee, entries, arguments.subspan(1, callee.parameter_count));
  builder_.CreateStore(trace_frame_, frame_address);
  builder_.CreateStore(depth, depth_address);
  Store(instruction.dst, result, callee.return_type);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(checked);
  return done;
}

void FunctionEmitter::EmitImport() {
  SetPosition();
  using AotCallKind = runtime::NativeFunction::AotCallKind;
  auto* execution = body_->getArg(0);
  auto* function = builder_.getInt32(static_cast<std::uint32_t>(function_index_));
  std::vector<llvm::Value*> parameters;
  for (std::size_t index = 0; index < source_.parameter_count; ++index) {
    const auto reg = static_cast<ir::RegisterId>(index);
    if (const auto slot = roots_.slots.at(reg)) {
      WriteHeapValue(RootAddress(*slot), reg, MemoryRegion::Roots);
    }
    const auto& type = source_.value_types.at(reg);
    parameters.push_back(type.kind == Kind::Dynamic || HasValidity(type) ? Pack(Encode(reg), index)
                                                                         : Load(reg));
  }
  if (program_.native_mode == NativeProgramMode::Reloadable || roots_.debug_values) {
    Poll();
  }
  EmitIntrinsicImport();
  const auto& return_type = source_.return_type;
  const bool packed_result = return_type.kind == Kind::Dynamic || HasValidity(return_type);
  auto* host_output = packed_result ? result_ : Output();
  llvm::Type* result_type = nullptr;
  if (packed_result) {
    result_type = NativeRecordType(module_.getContext());
  } else if (return_type.kind == Kind::Bool || return_type.kind == Kind::Void) {
    result_type = builder_.getInt8Ty();
  } else {
    result_type = ValueType(module_.getContext(), return_type);
  }
  auto* entries = body_->getArg(static_cast<unsigned>(source_.parameter_count + 1));
  auto* target = builder_.CreateExtractValue(entries, 0);
  auto* call_kind = builder_.CreateExtractValue(entries, 1);
  if (HasDirectHostResult(return_type)) {
    auto* direct = llvm::BasicBlock::Create(module_.getContext(), "import.direct", body_);
    auto* checked = llvm::BasicBlock::Create(module_.getContext(), "import.checked", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpNE(call_kind,
                              builder_.getInt8(static_cast<std::uint8_t>(AotCallKind::Checked))),
        direct, checked);
    builder_.SetInsertPoint(direct);
    auto* result = EmitDirectHostCall(source_, entries, parameters);
    if (return_type.kind != Kind::Void) {
      builder_.CreateStore(result, Output());
    }
    Finish(Status(builder_, ExecutionStatus::Returned));
    builder_.SetInsertPoint(checked);
  }
  std::vector<llvm::Value*> arguments{execution, function};
  std::vector<llvm::Type*> types{builder_.getPtrTy(), builder_.getInt32Ty()};
  for (auto* parameter : parameters) {
    auto* value = parameter->getType()->isIntegerTy(1)
                      ? builder_.CreateZExt(parameter, builder_.getInt8Ty())
                      : parameter;
    arguments.push_back(value);
    types.push_back(value->getType());
  }
  arguments.push_back(host_output);
  types.push_back(builder_.getPtrTy());
  auto* signature = llvm::FunctionType::get(builder_.getInt8Ty(), types, false);
  EmitHostBoundary(HostBorrow::Enter);
  auto* called = builder_.CreateCall(signature, target, arguments);
  EmitHostBoundary(HostBorrow::Exit);
  CheckStatus(called);
  if (return_type.kind != Kind::Void) {
    llvm::Value* value = builder_.CreateLoad(result_type, host_output);
    if (packed_result) {
      value = DecodeValue(builder_, value, return_type);
    } else if (return_type.kind == Kind::Bool) {
      value = builder_.CreateICmpNE(value, builder_.getInt8(0));
    }
    builder_.CreateStore(value, Output());
  }
  Finish(Status(builder_, ExecutionStatus::Returned));
}

}  // namespace vela::compiler::native
