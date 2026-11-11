#include "compiler/native/function_emitter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ModRef.h>

#include "base/callable.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/native/native_symbols.h"

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
  if (host_trace_active_ != nullptr) {
    return;
  }
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

void FunctionEmitter::BorrowHostExecution() {
  if (!IsClosedLinkedProgram(program_.native_mode, placement_) ||
      !ir::CanAnalyzeSynchronousFunction(source_) || !IsScalar(source_.return_type) ||
      import_frame_ == nullptr || !roots_.bounded_polls) {
    return;
  }
  for (const auto& instruction : source_.instructions) {
    if (instruction.opcode == Opcode::LoadString || instruction.opcode == Opcode::Return ||
        instruction.opcode == Opcode::GetGlobal) {
      continue;
    }
    if (instruction.opcode == Opcode::Call) {
      if (!CanInlineImportCall(program_.functions[instruction.index])) {
        return;
      }
    } else if (ir::AnalyzeEffects(instruction).MayCollectOnSuccess()) {
      return;
    }
  }
  // 宿主调用和惰性全局初始化都沿用当前执行，嵌套借用在返回前恢复 host_execution。
  // 其余指令只做纯计算或直接失败退出，因此整个函数可共享同一次执行借用。
  auto* active = ByteAddress(ControlState(), offsetof(runtime::NativeControlState, host_execution));
  auto* previous = builder_.CreateLoad(builder_.getPtrTy(), active);
  builder_.CreateStore(body_->getArg(0), active);
  Builder cleanup(cleanup_->getTerminator());
  cleanup.CreateStore(previous, active);
  Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
  host_trace_active_ = allocation.CreateAlloca(builder_.getInt1Ty(), nullptr, "host.trace.active");
  builder_.CreateStore(builder_.getFalse(), host_trace_active_);
}

void FunctionEmitter::EnterHostFrame(llvm::Value* depth) {
  llvm::BasicBlock* ready = nullptr;
  if (host_trace_active_ != nullptr) {
    auto* enter = llvm::BasicBlock::Create(module_.getContext(), "host.trace.enter", body_);
    ready = llvm::BasicBlock::Create(module_.getContext(), "host.trace.ready", body_);
    builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), host_trace_active_), ready,
                          enter);
    builder_.SetInsertPoint(enter);
  }
  auto* next_depth = builder_.CreateAdd(depth, builder_.getInt64(1));
  StoreExecutionState(builder_, import_frame_, next_depth);
  if (ready != nullptr) {
    builder_.CreateStore(builder_.getTrue(), host_trace_active_);
    builder_.CreateBr(ready);
    builder_.SetInsertPoint(ready);
  }
}

void FunctionEmitter::ReleaseBorrowedHostFrame() {
  if (host_trace_active_ == nullptr) {
    return;
  }
  auto* release = llvm::BasicBlock::Create(module_.getContext(), "host.trace.release", body_);
  auto* ready = llvm::BasicBlock::Create(module_.getContext(), "host.trace.released", body_);
  builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), host_trace_active_), release,
                        ready);
  builder_.SetInsertPoint(release);
  // 连续的宿主调用共用同一逻辑帧；真正的脚本观察边界恢复调用者，错误回溯不携带旧宿主帧。
  RestoreHostFrame();
  builder_.CreateStore(builder_.getFalse(), host_trace_active_);
  builder_.CreateBr(ready);
  builder_.SetInsertPoint(ready);
}

void FunctionEmitter::RestoreHostFrame() {
  auto* restored_depth = builder_.CreateAdd(entry_depth_, builder_.getInt64(1));
  StoreExecutionState(builder_, trace_frame_, restored_depth);
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

void FunctionEmitter::InitializeNativeStringView(llvm::Value* string, llvm::Value* storage) {
  auto initialize =
      module_.getOrInsertFunction(runtime::kNativeBorrowStringViewSymbol, builder_.getVoidTy(),
                                  builder_.getPtrTy(), builder_.getPtrTy());
  auto& function = *llvm::cast<llvm::Function>(initialize.getCallee());
  function.addFnAttr(llvm::Attribute::NoUnwind);
  function.addFnAttr(llvm::Attribute::WillReturn);
  function.addFnAttr(llvm::Attribute::NoSync);
  function.addFnAttr(llvm::Attribute::NoFree);
  function.setMemoryEffects(llvm::MemoryEffects::argMemOnly());
  function.addParamAttr(0, llvm::Attribute::ReadOnly);
  function.addParamAttr(1, llvm::Attribute::WriteOnly);
  const auto captures =
      llvm::Attribute::getWithCaptureInfo(module_.getContext(), llvm::CaptureInfo::none());
  function.addParamAttr(1, captures);
  builder_.CreateCall(initialize, {string, storage});
}

void FunctionEmitter::CacheNativeStringViews() {
  if (native_view_constants_.empty()) {
    return;
  }
  auto* opaque = llvm::FixedVectorType::get(builder_.getInt8Ty(),
                                            runtime::NativeMemoryLayout::kBorrowedStringViewBytes);
  for (const auto constant : native_view_constants_) {
    auto* storage = AllocateAuxiliary(runtime::NativeMemoryLayout::kBorrowedStringViewBytes);
    InitializeNativeStringView(constants_.at(constant), storage);
    auto* view = builder_.CreateLoad(opaque, storage);
    constant_string_views_.emplace(constant, view);
  }
}

llvm::Value* FunctionEmitter::EmitNativeStringArgument(llvm::Value* string,
                                                       std::size_t parameter,
                                                       llvm::Value* kind) {
  const auto* instruction =
      source_.native_symbol.empty() ? &source_.instructions.at(instruction_index_) : nullptr;
  auto* native =
      builder_.CreateICmpEQ(kind, builder_.getInt8(static_cast<std::uint8_t>(
                                      runtime::NativeFunction::AotCallKind::NativeStringViews)));
  llvm::Value* view = nullptr;
  if (instruction != nullptr && instruction->opcode == Opcode::Call &&
      !string_constant_registers_.empty()) {
    const auto constant = string_constant_registers_.at(instruction->lhs + parameter);
    if (constant) {
      if (const auto found = constant_string_views_.find(*constant);
          found != constant_string_views_.end()) {
        auto* copy = AllocateAuxiliary(runtime::NativeMemoryLayout::kBorrowedStringViewBytes);
        builder_.CreateStore(found->second, copy);
        view = copy;
      }
    }
  }
  if (view == nullptr) {
    view = AllocateAuxiliary(runtime::NativeMemoryLayout::kBorrowedStringViewBytes);
    auto& context = module_.getContext();
    auto* initialize = llvm::BasicBlock::Create(context, "host.string.view", body_);
    auto* ready = llvm::BasicBlock::Create(context, "host.string.ready", body_);
    builder_.CreateCondBr(native, initialize, ready);
    builder_.SetInsertPoint(initialize);
    InitializeNativeStringView(string, view);
    builder_.CreateBr(ready);
    builder_.SetInsertPoint(ready);
  }
  return builder_.CreateSelect(native, view, string);
}

std::size_t FunctionEmitter::PlanStringViewStorage() {
  using runtime::NativeMemoryLayout;
  string_constant_registers_ = FindStringConstants(source_, program_.functions);
  std::size_t views = 0;
  if (!source_.native_symbol.empty()) {
    views += static_cast<std::size_t>(
        std::ranges::count_if(std::span(source_.value_types).first(source_.parameter_count),
                              [](const auto& type) { return type.kind == Kind::String; }));
  }
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    const auto target = CallTarget(index);
    if (!target) {
      continue;
    }
    const auto& callee = program_.functions[target->function];
    if (!CanInlineImportCall(callee)) {
      continue;
    }
    const auto& instruction = source_.instructions[index];
    for (std::size_t parameter = 0; parameter < callee.parameter_count; ++parameter) {
      if (callee.value_types[parameter].kind != Kind::String) {
        continue;
      }
      ++views;
      if (instruction.opcode != Opcode::Call) {
        continue;
      }
      if (const auto constant = string_constant_registers_[instruction.lhs + parameter]) {
        native_view_constants_.push_back(*constant);
      }
    }
  }
  std::ranges::sort(native_view_constants_);
  const auto duplicates = std::ranges::unique(native_view_constants_);
  native_view_constants_.erase(duplicates.begin(), duplicates.end());
  return (views + native_view_constants_.size()) * NativeMemoryLayout::kBorrowedStringViewBytes;
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
    } else if (placement_ == NativeCodePlacement::Linked && type.kind == Kind::String) {
      parameters[index] = EmitNativeStringArgument(parameters[index], index,
                                                   builder_.CreateExtractValue(entries, 1));
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
  if (placement_ == NativeCodePlacement::Linked) {
    auto* native_context = parameters.front();
    auto* handles = builder_.CreateLoad(
        builder_.getPtrTy(),
        ByteAddress(native_context, runtime::NativeMemoryLayout::kContextHandles));
    auto* release = llvm::BasicBlock::Create(context, "host.handles.release", body_);
    auto* released = llvm::BasicBlock::Create(context, "host.handles.released", body_);
    builder_.CreateCondBr(builder_.CreateIsNotNull(handles), release, released,
                          llvm::MDBuilder(context).createUnlikelyBranchWeights());
    builder_.SetInsertPoint(release);
    auto cleanup = module_.getOrInsertFunction(runtime::kNativeReleaseHostHandlesSymbol,
                                               builder_.getVoidTy(), builder_.getPtrTy());
    builder_.CreateCall(cleanup, {native_context});
    builder_.CreateBr(released);
    builder_.SetInsertPoint(released);
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
    // AOT 的直接字符串 ABI 借用 StringObject*；调用者已有根覆盖宿主调用期限。
    if (placement_ == NativeCodePlacement::Linked && type.kind == Kind::String && !type.nullable) {
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
  EnterHostFrame(depth);
  auto* result = EmitDirectHostCall(callee, entries, arguments.subspan(1, callee.parameter_count));
  if (host_trace_active_ == nullptr) {
    RestoreHostFrame();
  }
  Store(instruction.dst, result, callee.return_type);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(checked);
  ReleaseBorrowedHostFrame();
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
      StoreRootValue(reg, RootAddress(*slot));
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
