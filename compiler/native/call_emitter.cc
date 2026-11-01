#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_image.h"
namespace vela::compiler::native {
bool FunctionEmitter::UsesEntrySlot(std::size_t callee) const {
  return tier_ == NativeCodeTier::Optimized &&
         calls_.UsesEntrySlot({.caller = function_index_, .callee = callee});
}

std::optional<CallableTarget> FunctionEmitter::CallTarget(std::size_t index) const {
  const auto& instruction = source_.instructions.at(index);
  if (instruction.opcode == Opcode::Call) {
    return CallableTarget{.function = instruction.index};
  }
  return direct_calls_.empty() ? std::nullopt : direct_calls_.at(index);
}

llvm::BasicBlock* FunctionEmitter::EmitSquareRootCall(const ir::Instruction& instruction,
                                                      std::uint32_t target) {
  const auto& callee = program_.functions.at(target);
  llvm::BasicBlock* intrinsic_done = nullptr;
  if (CanInlineSquareRoot(callee)) {
    auto* operand = Payload(instruction.lhs);
    auto* matches = SquareRootCondition(*imports_.at(target), operand);
    if (program_.native_mode == NativeProgramMode::Reloadable || roots_.debug_values) {
      matches = builder_.CreateAnd(
          matches, builder_.CreateICmpEQ(LoadSafepointRequests(), builder_.getInt8(0)));
    }
    auto& context = module_.getContext();
    auto* calculate = llvm::BasicBlock::Create(context, "call.sqrt", body_);
    auto* bound = llvm::BasicBlock::Create(context, "call.binding", body_);
    intrinsic_done = llvm::BasicBlock::Create(context, "call.sqrt.done", body_);
    builder_.CreateCondBr(matches, calculate, bound);
    builder_.SetInsertPoint(calculate);
    Store(instruction.dst, builder_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, operand),
          callee.return_type);
    builder_.CreateBr(intrinsic_done);
    builder_.SetInsertPoint(bound);
    if (program_.native_mode == NativeProgramMode::Reloadable &&
        !NeedsBoundaryPoll(instruction_index_)) {
      Poll();
    }
    PrepareRuntimeCall();
  }
  return intrinsic_done;
}

std::vector<llvm::Value*> FunctionEmitter::CallArguments(const ir::Instruction& instruction,
                                                         CallableTarget selected,
                                                         std::optional<std::size_t> borrowed) {
  const auto& callee = program_.functions.at(selected.function);
  std::vector<llvm::Value*> arguments{body_->getArg(0)};
  const auto closure = selected.mode == CallableMode::Closure;
  for (std::size_t index = 0; index < callee.parameter_count; ++index) {
    if (index == 0 && borrowed) {
      arguments.push_back(records_.at(*borrowed));
      continue;
    }
    const auto reg = closure && index == 0
                         ? instruction.rhs
                         : static_cast<ir::RegisterId>(instruction.lhs + index - (closure ? 1 : 0));
    arguments.push_back(
        Convert(builder_, Load(reg), source_.value_types.at(reg), callee.value_types.at(index)));
  }
  if (!callee.native_symbol.empty()) {
    arguments.push_back(imports_.at(selected.function));
  }
  // 各类结果都可放入对齐的单个 ABI 暂存槽；循环和多调用不增加帧大小。
  arguments.push_back(result_);
  return arguments;
}

void FunctionEmitter::EmitCall(const ir::Instruction& instruction, CallableTarget selected) {
  // 即使被调用方使用延迟叶入口，调用者也必须占用深度预算并保留失败时的调用链。
  PrepareTraceFrame();
  const auto& callee = program_.functions.at(selected.function);
  auto* intrinsic_done = EmitSquareRootCall(instruction, selected.function);
  const auto borrowed = escape_.borrowed_calls.empty()
                            ? std::optional<std::size_t>{}
                            : escape_.borrowed_calls.at(instruction_index_);
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
    CompleteCall(builder_.CreateCall(helper,
                                     {
                                         body_->getArg(0),
                                         builder_.getInt64(callee.identity.declaration.value),
                                         arguments_,
                                         builder_.getInt64(callee.parameter_count),
                                         result_,
                                     }),
                 instruction.dst);
    return;
  }
  auto arguments = CallArguments(instruction, selected, borrowed);
  const auto& target = generated_.subspan(selected.function).front();
  llvm::Value* status = nullptr;
  if (UsesEntrySlot(selected.function)) {
    // 槽属于本次发布的不可变映像；复用的机器码从执行实例读取新目标。
    // 旧帧在安全点恢复到基线后按声明调用，因此不借此进入旧快照的命名目标。
    using runtime::NativeFunctionDescription;
    using runtime::NativeImage;
    auto* functions = builder_.CreateLoad(builder_.getPtrTy(),
                                          ByteAddress(image_, offsetof(NativeImage, functions)));
    const auto offset = (sizeof(NativeFunctionDescription) * selected.function) +
                        offsetof(NativeFunctionDescription, typed_entry);
    auto* entry = builder_.CreateLoad(builder_.getPtrTy(), ByteAddress(functions, offset),
                                      "call.typed.entry");
    status = builder_.CreateCall(target.body->getFunctionType(), entry, arguments);
  } else if (target.leaf != nullptr) {
    auto& context = module_.getContext();
    auto* leaf = llvm::BasicBlock::Create(context, "call.leaf", body_);
    auto* observable = llvm::BasicBlock::Create(context, "call.observable", body_);
    auto* merge = llvm::BasicBlock::Create(context, "call.done", body_);
    auto* failed = llvm::BasicBlock::Create(context, "call.leaf.failed", body_);
    auto* returned = llvm::BasicBlock::Create(context, "call.leaf.returned", body_);
    // 调试器及控制请求需要真正的被调用帧；无请求时只执行有界的纯计算。
    llvm::Value* ready = builder_.CreateICmpEQ(LoadSafepointRequests(), builder_.getInt8(0));
    if (target.leaf_guard != nullptr) {
      ready = builder_.CreateAnd(ready, builder_.CreateCall(target.leaf_guard, arguments));
    }
    builder_.CreateCondBr(ready, leaf, observable,
                          llvm::MDBuilder(context).createLikelyBranchWeights());
    builder_.SetInsertPoint(leaf);
    auto* computed = builder_.CreateCall(target.leaf, arguments);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(computed, Status(builder_, ExecutionStatus::Returned)), returned,
        failed, llvm::MDBuilder(context).createLikelyBranchWeights());
    builder_.SetInsertPoint(failed);
    PrepareObservableCall();
    auto* position = builder_.CreateLoad(builder_.getInt64Ty(), result_);
    auto failure = module_.getOrInsertFunction(
        "vela_native_v2_leaf_failure", builder_.getInt8Ty(), builder_.getPtrTy(),
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getPtrTy(), builder_.getInt64Ty());
    auto* materialized =
        builder_.CreateCall(failure, {
                                         body_->getArg(0),
                                         position,
                                         computed,
                                         llvm::ConstantPointerNull::get(builder_.getPtrTy()),
                                         builder_.getInt64(0),
                                     });
    Fail(materialized);
    builder_.SetInsertPoint(returned);
    builder_.CreateBr(merge);
    builder_.SetInsertPoint(observable);
    PrepareObservableCall();
    auto* called = builder_.CreateCall(target.body, arguments);
    CheckStatus(called);
    if (callee.return_type.kind != Kind::Void) {
      Store(instruction.dst,
            builder_.CreateLoad(ValueType(module_.getContext(), callee.return_type), result_),
            callee.return_type);
    }
    PollAfterRuntimeCall();
    auto* observed = builder_.GetInsertBlock();
    builder_.CreateBr(merge);
    builder_.SetInsertPoint(merge);
    auto* outcome = builder_.CreatePHI(builder_.getInt8Ty(), 2);
    outcome->addIncoming(computed, returned);
    outcome->addIncoming(called, observed);
    status = outcome;
  } else {
    status = builder_.CreateCall(borrowed ? target.borrowed : target.body, arguments);
  }
  if (borrowed) {
    // 被调用方可能在抛错前修改引用字段；错误封套分配前刷新调用者根。
    StoreRoots(instruction_index_);
  }
  CheckStatus(status);
  if (callee.return_type.kind == Kind::Void) {
    Store(instruction.dst, builder_.getFalse(), callee.return_type);
  } else {
    Store(instruction.dst,
          builder_.CreateLoad(ValueType(module_.getContext(), callee.return_type), result_),
          callee.return_type);
  }
  if (intrinsic_done != nullptr) {
    PollAfterRuntimeCall();
    builder_.CreateBr(intrinsic_done);
    builder_.SetInsertPoint(intrinsic_done);
  }
}

}  // namespace vela::compiler::native
