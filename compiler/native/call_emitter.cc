#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_symbols.h"
#include "runtime/value/object.h"
namespace vela::compiler::native {
bool FunctionEmitter::IsEntrySlotRequired(std::size_t callee) const {
  return tier_ == NativeCodeTier::Optimized &&
         calls_.IsEntrySlotRequired({.caller = function_index_, .callee = callee});
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
  if (generated_[target].square_root) {
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
    PrepareInlineRuntimeCall();
  }
  return intrinsic_done;
}

std::vector<llvm::Value*> FunctionEmitter::CallArguments(const ir::Instruction& instruction,
                                                         CallableTarget selected,
                                                         std::optional<std::size_t> borrowed) {
  const auto& callee = program_.functions.at(selected.function);
  std::vector<llvm::Value*> arguments{body_->getArg(0)};
  const auto closure = selected.mode == CallableMode::Closure;
  const auto bound = selected.mode == CallableMode::BoundMethod;
  for (std::size_t index = 0; index < callee.parameter_count; ++index) {
    if (index == 0 && borrowed) {
      arguments.push_back(records_.at(*borrowed));
      continue;
    }
    if (bound && index == 0) {
      // 目标来自当前函数中单次定义的 callable；第一个字段是已保根的固定 receiver。
      arguments.push_back(LoadMemory(
          builder_.getPtrTy(), ByteAddress(Reference(instruction.rhs), sizeof(runtime::HeapObject)),
          MemoryRegion::FieldPayloads));
      continue;
    }
    const auto reg =
        closure && index == 0
            ? instruction.rhs
            : static_cast<ir::RegisterId>(instruction.lhs + index - ((closure || bound) ? 1 : 0));
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

llvm::Value* FunctionEmitter::EmitLeafCall(const ir::Instruction& instruction,
                                           CallableTarget selected,
                                           std::span<llvm::Value* const> arguments) {
  const auto& target = generated_[selected.function];
  const auto& callee = program_.functions.at(selected.function);
  auto* result = arguments.back();
  auto& context = module_.getContext();
  auto* leaf = llvm::BasicBlock::Create(context, "call.leaf", body_);
  auto* observable = llvm::BasicBlock::Create(context, "call.observable", body_);
  auto* merge = llvm::BasicBlock::Create(context, "call.done", body_);
  auto* failed = llvm::BasicBlock::Create(context, "call.leaf.failed", body_);
  auto* returned = llvm::BasicBlock::Create(context, "call.leaf.returned", body_);
  // 调试器及控制请求需要真正的被调用帧；无请求时只执行有界的纯计算。
  llvm::Value* ready = builder_.CreateICmpEQ(LoadSafepointRequests(), builder_.getInt8(0));
  if (target.budgeted_recursion) {
    // 纯计算最多展开固定深度；一次预留逻辑和物理栈预算，内层不维护可观察帧。
    auto* depth = builder_.CreateAdd(entry_depth_, builder_.getInt64(kRecursiveLeafDepth + 1));
    ready = builder_.CreateAnd(
        ready, HasStackBudget(depth, kMaximumNativeStackBytes * kRecursiveLeafDepth));
  }
  if (target.leaf_guard != nullptr) {
    ready = builder_.CreateAnd(ready, builder_.CreateCall(target.leaf_guard, arguments));
  }
  auto* branch = builder_.CreateCondBr(builder_.CreateNot(ready), observable, leaf,
                                       llvm::MDBuilder(context).createUnlikelyBranchWeights());
  if (target.leaf_guard == nullptr && !target.budgeted_recursion) {
    // 纯叶调用沿用循环的有界轮询预算；异常分支仍建立完整调用链。
    branch->setMetadata(kNativeBoundedPollMetadata, llvm::MDNode::get(context, {}));
  }
  builder_.SetInsertPoint(leaf);
  std::vector<llvm::Value*> leaf_arguments(arguments.begin(), arguments.end());
  if (target.budgeted_recursion) {
    leaf_arguments.insert(leaf_arguments.end() - 1, builder_.getInt32(kRecursiveLeafDepth));
  }
  auto* computed = builder_.CreateCall(target.leaf, leaf_arguments);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(computed, Status(builder_, ExecutionStatus::Returned)), returned,
      failed, llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(failed);
  if (target.budgeted_recursion) {
    // 只重放无状态的纯计算；预算耗尽和算术失败都由完整入口建立真实错误调用链。
    builder_.CreateBr(observable);
  } else {
    PrepareObservableCall();
    auto* position = builder_.CreateLoad(builder_.getInt64Ty(), result);
    auto failure = module_.getOrInsertFunction(
        runtime::kNativeLeafFailureSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
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
  }
  builder_.SetInsertPoint(returned);
  builder_.CreateBr(merge);
  builder_.SetInsertPoint(observable);
  PrepareObservableCall();
  auto* called = builder_.CreateCall(target.body, arguments);
  CheckStatus(called);
  if (callee.return_type.kind != Kind::Void) {
    Store(instruction.dst,
          builder_.CreateLoad(ValueType(module_.getContext(), callee.return_type), result),
          callee.return_type);
  }
  PollAfterRuntimeCall();
  auto* observed = builder_.GetInsertBlock();
  builder_.CreateBr(merge);
  builder_.SetInsertPoint(merge);
  auto* outcome = builder_.CreatePHI(builder_.getInt8Ty(), 2);
  outcome->addIncoming(computed, returned);
  outcome->addIncoming(called, observed);
  return outcome;
}

void FunctionEmitter::EmitCall(const ir::Instruction& instruction, CallableTarget selected) {
  const auto& callee = program_.functions.at(selected.function);
  const auto& target = generated_[selected.function];
  if (kind_ == NativeEntryKind::Leaf && target.budgeted_recursion &&
      selected.function == function_index_) {
    auto arguments = CallArguments(instruction, selected, std::nullopt);
    auto* budget = body_->getArg(static_cast<unsigned>(source_.parameter_count + 1));
    arguments.insert(arguments.end() - 1, builder_.CreateNUWSub(budget, builder_.getInt32(1)));
    CheckStatus(builder_.CreateCall(target.leaf, arguments));
    if (callee.return_type.kind == Kind::Void) {
      Store(instruction.dst, builder_.getFalse(), callee.return_type);
    } else {
      Store(instruction.dst,
            builder_.CreateLoad(ValueType(module_.getContext(), callee.return_type), result_),
            callee.return_type);
    }
    return;
  }
  // 即使被调用方使用延迟叶入口，调用者也必须占用深度预算并保留失败时的调用链。
  PrepareTraceFrame();
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
        runtime::kNativeCallDeclarationSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
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
  auto* result = target.leaf == nullptr ? result_ : leaf_result_;
  arguments.back() = result;
  auto* import_done = EmitInlineImportCall(instruction, selected, arguments);
  llvm::Value* status = nullptr;
  if (IsEntrySlotRequired(selected.function)) {
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
    status = EmitLeafCall(instruction, selected, arguments);
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
          builder_.CreateLoad(ValueType(module_.getContext(), callee.return_type), result),
          callee.return_type);
  }
  if (import_done != nullptr) {
    builder_.CreateBr(import_done);
    builder_.SetInsertPoint(import_done);
  }
  if (intrinsic_done != nullptr) {
    PollAfterRuntimeCall();
    builder_.CreateBr(intrinsic_done);
    builder_.SetInsertPoint(intrinsic_done);
  }
}

}  // namespace vela::compiler::native
