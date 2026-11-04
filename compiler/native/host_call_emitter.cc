#include "compiler/native/function_emitter.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <llvm/IR/BasicBlock.h>
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
  return type.kind == Kind::Void || type.kind == Kind::Bool || type.kind == Kind::Int ||
         type.kind == Kind::Double;
}

}  // namespace

void FunctionEmitter::EmitHostBoundary(HostBorrow change) {
  if (program_.native_mode != NativeProgramMode::Reloadable) {
    return;
  }
  auto* address = ByteAddress(ControlState(), offsetof(runtime::NativeControlState, host_borrows));
  auto* depth = builder_.CreateLoad(builder_.getInt64Ty(), address);
  auto* updated = change == HostBorrow::Enter ? builder_.CreateAdd(depth, builder_.getInt64(1))
                                              : builder_.CreateSub(depth, builder_.getInt64(1));
  builder_.CreateStore(updated, address);
}

llvm::Value* FunctionEmitter::EmitDirectHostCall(const ir::Function& callee,
                                                 llvm::Value* entries,
                                                 std::span<llvm::Value* const> arguments) {
  using AotCallKind = runtime::NativeFunction::AotCallKind;
  auto& context = module_.getContext();
  const auto& return_type = callee.return_type;
  const auto return_kind = return_type.kind;
  auto* result_type = builder_.getVoidTy();
  if (return_kind != Kind::Void) {
    result_type =
        return_kind == Kind::Bool ? builder_.getInt8Ty() : ValueType(context, return_type);
  }
  auto* target = builder_.CreateExtractValue(entries, 0);
  auto* call_kind = builder_.CreateExtractValue(entries, 1);
  auto* with_context = llvm::BasicBlock::Create(context, "import.context", body_);
  auto* plain = llvm::BasicBlock::Create(context, "import.plain", body_);
  auto* done = llvm::BasicBlock::Create(context, "import.returned", body_);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(
          call_kind, builder_.getInt8(static_cast<std::uint8_t>(AotCallKind::DirectWithContext))),
      with_context, plain);
  const auto emit = [&](AotCallKind kind) {
    std::vector<llvm::Value*> values(arguments.begin(), arguments.end());
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (callee.value_types.at(index).kind == Kind::Bool) {
        values.at(index) = builder_.CreateZExt(values.at(index), builder_.getInt8Ty());
      }
    }
    if (kind == AotCallKind::DirectWithContext) {
      values.insert(values.begin(),
                    ByteAddress(body_->getArg(0), runtime::NativeMemoryLayout::kExecutionContext));
    }
    std::vector<llvm::Type*> types;
    types.reserve(values.size());
    for (auto* value : values) {
      types.push_back(value->getType());
    }
    auto* signature = llvm::FunctionType::get(result_type, types, false);
    EmitHostBoundary(HostBorrow::Enter);
    llvm::Value* result = builder_.CreateCall(signature, target, values);
    EmitHostBoundary(HostBorrow::Exit);
    if (return_kind == Kind::Bool) {
      result = builder_.CreateICmpNE(result, builder_.getInt8(0));
    }
    builder_.CreateBr(done);
    return result;
  };
  builder_.SetInsertPoint(with_context);
  auto* contextual = emit(AotCallKind::DirectWithContext);
  builder_.SetInsertPoint(plain);
  auto* ordinary = emit(AotCallKind::Direct);
  builder_.SetInsertPoint(done);
  if (return_kind == Kind::Void) {
    return builder_.getFalse();
  }
  auto* result = builder_.CreatePHI(contextual->getType(), 2, "import.result");
  result->addIncoming(contextual, with_context);
  result->addIncoming(ordinary, plain);
  return result;
}

bool FunctionEmitter::CanInlineImportCall(const ir::Function& callee) const {
  if (program_.native_mode != NativeProgramMode::Closed || roots_.debug_values ||
      callee.native_symbol.empty() || !HasDirectHostResult(callee.return_type) ||
      callee.return_type.nullable) {
    return false;
  }
  // 调用点的根计划包含全部托管实参，包括最后一次使用的 receiver。直接入口的
  // host codec 从包装对象固定位置借用指针，原生调用期间仍保留调用者的根登记。
  const auto& signature = *callee.signature;
  const auto receiver_count = static_cast<std::size_t>(signature.receiver.has_value());
  for (std::size_t index = 0; index < callee.parameter_count; ++index) {
    const auto& type = callee.value_types[index];
    if (IsScalar(type) && !type.nullable) {
      continue;
    }
    if (type.kind != Kind::Class) {
      return false;
    }
    const auto type_index = receiver_count != 0 && index == 0
                                ? *signature.receiver
                                : signature.parameters[index - receiver_count].type;
    const auto& type_layout = program_.layouts.types[type_index];
    if (program_.layouts.classes[type_layout.index].kind != layout::Class::Kind::Host) {
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
  auto* depth = builder_.CreateLoad(builder_.getInt64Ty(), depth_address);
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
    parameters.push_back(Load(reg));
  }
  if (program_.native_mode == NativeProgramMode::Reloadable || roots_.debug_values) {
    Poll();
  }
  EmitIntrinsicImport();
  const auto& return_type = source_.return_type;
  auto* result_type = return_type.kind == Kind::Bool || return_type.kind == Kind::Void
                          ? builder_.getInt8Ty()
                          : ValueType(module_.getContext(), return_type);
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
  arguments.push_back(Output());
  types.push_back(builder_.getPtrTy());
  auto* signature = llvm::FunctionType::get(builder_.getInt8Ty(), types, false);
  EmitHostBoundary(HostBorrow::Enter);
  auto* called = builder_.CreateCall(signature, target, arguments);
  EmitHostBoundary(HostBorrow::Exit);
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

}  // namespace vela::compiler::native
