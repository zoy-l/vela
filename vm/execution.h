#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "base/callable.h"
#include "base/execution_limits.h"
#include "base/member.h"
#include "runtime/exception.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/json.h"
#include "runtime/object.h"
#include "runtime/register_storage.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/debugger.h"
#include "vm/execution_status.h"
#include "vm/frame.h"
#include "vm/program.h"
#include "vm/register_file.h"
#include "vm/vm.h"

namespace vela::vm::internal {

using StepResult = std::expected<void, ExecutionStatus>;

struct DebugSession final {
  Debugger* debugger = nullptr;
};

struct ActiveFrame final {
  Frame frame;
  const FunctionStorage* function = nullptr;
  std::size_t program_counter = 0;
  std::uint8_t return_register = 0;
  std::optional<std::size_t> initialized_global;
  // 固定帧数组为 encoder 及其 intrusive roots 提供稳定地址；只在 stringify 期间构造。
  std::optional<runtime::JsonEncoder> json_encoder;
  std::uint8_t json_destination = 0;
  bool dynamic_return = false;
  bool dynamic_call_plan = false;
  std::optional<std::uint8_t> dynamic_assignment = std::nullopt;
};

struct FutureRegisters final {
  std::uint8_t destination = 0;
  std::uint8_t source = 0;
  std::uint8_t layout = 0;
};

struct AsyncArguments final {
  AsyncOperation operation = AsyncOperation::Count;
  runtime::RegisterView values;
  runtime::RegisterReference destination;
  std::optional<runtime::TypeConstraint> runtime_type = std::nullopt;
  [[nodiscard]] runtime::Value At(std::size_t index) const {
    return values.subspan(index).front().Read();
  }
  StepResult StoreObject(runtime::HeapObject* object) const {
    if (object == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    auto result = destination;
    result.SetObject(object);
    return {};
  }
};

struct SuspensionPoint final {
  std::uint32_t function = 0;
  std::size_t program_counter = 0;
  std::uint8_t destination = 0;
};

class Execution final {
 public:
  Execution(runtime::Isolate& isolate,
            const Program& program,
            std::span<RegisterFile> registers,
            std::shared_ptr<DebugSession> debug_session = {})
      : isolate_(isolate),
        program_(program),
        functions_(program.functions()),
        registers_(registers),
        globals_(program.globals()),
        debug_session_(std::move(debug_session)) {}

  ExecutionResult Run(std::uint32_t entry_function) noexcept;
  ExecutionResult DebugRun(std::uint32_t entry_function, Debugger& debugger) noexcept;
  ExecutionResult ResumeFrame(SuspensionPoint point,
                              std::span<const std::uint8_t> indices,
                              std::span<const runtime::Value> saved,
                              const runtime::FutureResult& completion) noexcept;
  ExecutionResult Invoke(runtime::Value callable,
                         std::span<const runtime::Value> arguments) noexcept;
  [[nodiscard]] static runtime::FutureDiagnostic Diagnostic(const ExecutionResult& result);
  static void CompleteFuture(runtime::Isolate& isolate,
                             runtime::Future& future,
                             const ExecutionResult& result);
  static void CompleteFuture(runtime::Isolate& isolate,
                             runtime::Future& future,
                             const runtime::Future& source);
  [[nodiscard]] runtime::FutureDiagnostic ThrowDiagnostic(runtime::Value payload) const;
  [[nodiscard]] static runtime::Future InvokeFuture(
      runtime::Isolate& isolate,
      const Program& program,
      runtime::Value callable,
      std::span<const runtime::Value> arguments = {},
      std::shared_ptr<DebugSession> debug_session = {});

 private:
  enum class DynamicCallStyle : std::uint8_t { Function, Method };
  struct ResolvedCallable final {
    std::uint32_t function = 0;
    std::optional<runtime::Value> implicit = std::nullopt;
  };
  struct FrameEntry final {
    std::uint32_t function = 0;
    std::uint8_t destination = 0;
  };
  struct BoundArguments final {
    std::array<std::uint32_t, kMaxFrameRegisters> source_indices{};
    std::uint32_t count = 0;
    std::uint32_t skipped = 0;
    std::uint32_t rest_start = 0;
    std::uint32_t rest_count = 0;
  };
  struct ArgumentBinding final {
    BoundArguments bound;
    std::array<bool, kMaxFrameRegisters> assigned{};
    std::uint32_t positional = 0;
    bool named = false;
  };
  [[nodiscard]] static StepResult BindDynamicArgument(const CallSignature& signature,
                                                      std::string_view name,
                                                      std::uint32_t index,
                                                      DynamicCallStyle style,
                                                      ArgumentBinding& binding);
  [[nodiscard]] static StepResult CheckDynamicArguments(
      const CallSignature& signature,
      std::span<const runtime::TypeConstraint> types,
      const runtime::HeapObject& arguments,
      const BoundArguments& bound);
  [[nodiscard]] StepResult CopyDynamicArguments(const CallSignature& signature,
                                                std::span<const runtime::TypeConstraint> types,
                                                const runtime::HeapObject& arguments,
                                                const BoundArguments& bound,
                                                runtime::RegisterView parameters);
  [[nodiscard]] std::expected<runtime::HeapObject*, ExecutionStatus> PackDynamicRest(
      const runtime::HeapObject& arguments,
      const BoundArguments& bound,
      const runtime::ArrayLayout& type);
  [[nodiscard]] std::expected<ResolvedCallable, ExecutionStatus> ResolveCallable(
      runtime::Value value) const;
  [[nodiscard]] static std::expected<BoundArguments, ExecutionStatus> BindDynamicArguments(
      const CallSignature& signature,
      const ResolvedCallable& callable,
      const runtime::HeapObject& arguments,
      const runtime::HeapObject& names,
      DynamicCallStyle style);
  StepResult EnterDynamicCall(ActiveFrame& active,
                              const ResolvedCallable& callable,
                              const runtime::HeapObject& arguments,
                              const runtime::HeapObject& names,
                              std::uint8_t destination,
                              DynamicCallStyle style);
  template <bool Debugging>
  ExecutionResult RunImpl(std::uint32_t entry_function, Debugger* debugger) noexcept;
  template <bool Debugging>
  ExecutionResult Continue(Debugger* debugger) noexcept;
  // 保持热分派与调用/返回在同一优化边界；Windows ABI 的寄存器视图实参传递曾导致明显回归。
  [[clang::always_inline]] StepResult ExecuteInstruction(ActiveFrame& active,
                                                         Instruction instruction);
  [[clang::always_inline]] StepResult ExecuteObject(ActiveFrame& active,
                                                    Instruction instruction,
                                                    OpCode opcode,
                                                    std::uint8_t operand_a,
                                                    std::uint8_t operand_b,
                                                    std::uint8_t operand_c);
  StepResult AllocateObject(ActiveFrame& active,
                            OpCode opcode,
                            std::uint8_t operand_a,
                            std::uint8_t operand_b,
                            std::uint8_t operand_c);
  StepResult AllocateArrayOrInstance(ActiveFrame& active, OpCode opcode, FutureRegisters registers);
  StepResult AllocateFuture(ActiveFrame& active,
                            FutureRegisters registers,
                            std::uint8_t error_flag);
  StepResult AllocateSleep(ActiveFrame& active, FutureRegisters registers);
  StepResult AllocateCollection(ActiveFrame& active, FutureRegisters registers);
  StepResult Await(ActiveFrame& active, FutureRegisters registers);
  StepResult StringOperation(OpCode opcode,
                             runtime::RegisterReference destination,
                             runtime::Value left,
                             runtime::Value right);
  StepResult ArrayOperation(OpCode opcode,
                            runtime::RegisterReference destination,
                            runtime::Value left,
                            runtime::Value right);
  StepResult ExecuteBuiltin(ActiveFrame& active, Instruction instruction);
  static StepResult ExecuteTypeBinding(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteCalendar(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteJson(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamic(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicBinary(runtime::RegisterReference destination,
                                  runtime::RegisterView arguments);
  static StepResult ExecuteDynamicEquality(OpCode operation,
                                           runtime::RegisterReference destination,
                                           runtime::Value left,
                                           runtime::Value right);
  StepResult ExecuteDynamicIndex(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicArrayIndex(Builtin builtin,
                                      runtime::RegisterReference destination,
                                      runtime::RegisterView arguments);
  StepResult ExecuteDynamicCall(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicMember(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicMethodCall(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicArray(ActiveFrame& active, Instruction instruction);
  StepResult PrepareDynamicArray(ActiveFrame& active, Instruction instruction);
  StepResult InvokeDynamicArray(ActiveFrame& active, Instruction instruction);
  StepResult ReadDynamicMember(ActiveFrame& active,
                               Instruction instruction,
                               const MemberDefinition& member);
  StepResult WriteDynamicMember(ActiveFrame& active,
                                Instruction instruction,
                                const MemberDefinition& member);
  [[nodiscard]] std::expected<runtime::Value, ExecutionStatus> CreateDynamicCallPlan(
      runtime::Value value,
      DynamicCallStyle style,
      std::optional<std::uint32_t> target = std::nullopt);
  [[nodiscard, clang::always_inline]] std::expected<runtime::RegisterView, ExecutionStatus>
  EnterFrame(FrameEntry entry);
  [[nodiscard]] bool IsErasedReturn() const;
  [[nodiscard]] std::expected<runtime::Value, ExecutionStatus> BoxDynamicValue(
      runtime::Value value,
      const runtime::TypeConstraint& type);
  StepResult ContinueJson(ActiveFrame& active);
  StepResult JsonError(const runtime::JsonRuntimeError& error);
  StepResult ExecuteRegExp(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteAsync(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicAsync(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteCompleter(const AsyncArguments& call);
  StepResult ExecuteAsyncState(const AsyncArguments& call);
  StepResult ExecuteAsyncComputation(const AsyncArguments& call);
  StepResult ExecuteAsyncAggregation(ActiveFrame& active, const AsyncArguments& call);
  StepResult ExecuteAsyncComposition(const AsyncArguments& call);
  StepResult ExecuteNative(ActiveFrame& active);
  StepResult CompleteCallSetup();
  [[nodiscard]] std::expected<runtime::Value, ExecutionStatus> ValidateNativeFuture(
      runtime::Value value,
      std::optional<runtime::TypeConstraint> payload_type);
  StepResult Interpolate(ActiveFrame& active, Instruction instruction);
  static void Jump(ActiveFrame& active, std::int16_t offset) noexcept;
  [[clang::always_inline]] StepResult Call(ActiveFrame& active, Instruction instruction);
  StepResult CallIndirect(ActiveFrame& active, Instruction instruction);
  StepResult CallInterface(ActiveFrame& active, Instruction instruction);
  [[nodiscard]] std::expected<std::uint32_t, ExecutionStatus> InterfaceTarget(
      runtime::Value receiver,
      std::uint32_t selector) const;
  [[clang::always_inline]] StepResult EnterCall(
      std::uint32_t target,
      ActiveFrame& active,
      Instruction instruction,
      std::optional<runtime::Value> implicit,
      std::optional<std::size_t> initialized_global = std::nullopt);
  StepResult AccessGlobal(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteExpected(ActiveFrame& active, Instruction instruction);
  [[clang::always_inline]] StepResult Return(ActiveFrame& active, runtime::Value value);
  [[nodiscard]] std::vector<StackFrame> CaptureStack() const;
  [[nodiscard]] std::vector<StackFrame> ResolveTrace(
      std::span<const runtime::TracePosition> positions) const;
  [[nodiscard]] std::span<const runtime::TracePosition> CapturePositions(
      std::span<runtime::TracePosition> storage) const;
  std::expected<runtime::Value, ExecutionStatus> RuntimeError(ExecutionStatus status,
                                                              std::string_view message);
  StepResult CreateException(runtime::Value payload,
                             ExecutionStatus status,
                             std::string_view message);
  ExecutionStatus RestoreFutureError(const runtime::FutureResult& completion);
  std::optional<ExecutionResult> CompleteAsyncError(ActiveFrame& active);
  std::optional<ExecutionResult> HandleError(ExecutionStatus status);
  StepResult ExecuteException(ActiveFrame& active, Instruction instruction);
  void ResetInitializer(ActiveFrame& active) noexcept;
  void AbortInitializers() noexcept;

  runtime::Isolate& isolate_;
  const Program& program_;
  std::span<const FunctionStorage> functions_;
  std::span<RegisterFile> registers_;
  std::array<ActiveFrame, kMaxCallFrames> frames_;
  std::size_t frame_count_ = 0;
  runtime::Value result_;
  GlobalStorage* globals_;
  std::string error_message_;
  runtime::Value pending_exception_;
  runtime::RootRange exception_root_;
  std::shared_ptr<DebugSession> debug_session_;
};

}  // namespace vela::vm::internal
