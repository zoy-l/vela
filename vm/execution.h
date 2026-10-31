#pragma once

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
#include "base/callable.h"
#include "base/member.h"
#include "base/mock_operation.h"
#include "runtime/async/future.h"
#include "runtime/async/future_composition.h"
#include "runtime/dynamic/dynamic_call.h"
#include "runtime/exceptions/exception.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/json/json.h"
#include "runtime/value/callable.h"
#include "runtime/value/object.h"
#include "runtime/value/register_storage.h"
#include "runtime/value/value.h"
#include "vm/bytecode.h"
#include "vm/debug_info.h"
#include "vm/debugger.h"
#include "vm/execution_status.h"
#include "vm/frame.h"
#include "vm/frame_slots.h"
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
  Instruction index_prefix = 0;
  std::uint8_t return_register = 0;
  std::optional<std::size_t> initialized_global;
  // 帧槽分配后地址固定，encoder 的 intrusive roots 在 stringify 期间借用该地址。
  std::optional<runtime::JsonEncoder> json_encoder;
  std::uint8_t json_destination = 0;
  bool dynamic_return = false;
  bool dynamic_call_plan = false;
  std::optional<std::uint8_t> dynamic_assignment = std::nullopt;
};

struct FutureRegisters final {
  std::uint8_t destination = 0;
  std::uint8_t source = 0;
  std::uint32_t layout = 0;
};

struct SuspensionPoint final {
  std::uint32_t function = 0;
  std::size_t program_counter = 0;
  std::uint8_t destination = 0;
};

class SuspendedFrame;

class Execution final {
 public:
  Execution(runtime::Isolate& isolate,
            const Program& program,
            FrameSlots<RegisterFile>& registers,
            std::shared_ptr<DebugSession> debug_session = {})
      : isolate_(isolate),
        program_(program),
        functions_(program.functions()),
        registers_(registers),
        globals_(program.globals()),
        execution_resource_(isolate.external_resources(), runtime::ExternalResourceKind::Execution),
        debug_session_(std::move(debug_session)) {}

  ExecutionResult Run(std::uint32_t entry_function) noexcept;
  ExecutionResult DebugRun(std::uint32_t entry_function, Debugger& debugger) noexcept;
  std::optional<ExecutionResult> ResumeFrame(SuspendedFrame& suspended,
                                             const runtime::FutureResult& completion) noexcept;
  ExecutionResult Invoke(runtime::Value callable,
                         std::span<const runtime::Value> arguments) noexcept;
  [[nodiscard]] static runtime::FutureDiagnostic Diagnostic(const ExecutionResult& result);
  static void CompleteFuture(runtime::Isolate& isolate,
                             runtime::Future& future,
                             const ExecutionResult& result);
  [[nodiscard]] runtime::FutureDiagnostic ThrowDiagnostic(runtime::Value payload) const;
  [[nodiscard]] static runtime::Future InvokeFuture(
      runtime::Isolate& isolate,
      const Program& program,
      runtime::Value callable,
      std::span<const runtime::Value> arguments = {},
      std::shared_ptr<DebugSession> debug_session = {});

 private:
  using DynamicCallStyle = runtime::DynamicCallStyle;
  using ResolvedCallable = runtime::ResolvedCallable;
  [[clang::always_inline]] static StepResult StoreResult(
      runtime::RegisterReference destination,
      const std::expected<runtime::Value, ExecutionStatus>& result) {
    if (!result) {
      return std::unexpected(result.error());
    }
    destination = *result;
    return {};
  }
  template <class Error>
  StepResult StoreDiagnosticResult(runtime::RegisterReference destination,
                                   const std::expected<runtime::Value, Error>& result) {
    return StoreResult(destination, result.transform_error([this](const Error& error) {
      error_message_ = error.message;
      return error.status;
    }));
  }
  struct FrameEntry final {
    std::uint32_t function = 0;
    std::uint8_t destination = 0;
  };
  struct MockInspection final {
    MockOperation operation;
    runtime::Value signature;
    runtime::Value value;
    runtime::RegisterReference destination;
  };
  StepResult CopyDynamicArguments(const CallSignature& signature,
                                  std::span<const runtime::TypeConstraint> types,
                                  const runtime::HeapObject& arguments,
                                  const runtime::BoundArguments& bound,
                                  runtime::RegisterView parameters);
  [[nodiscard]] std::expected<ResolvedCallable, ExecutionStatus> ResolveCallable(
      runtime::Value value) const;
  // 借用当前不可变程序的签名；Execution 的程序所有权覆盖此视图的使用期。
  struct CallableContract final {
    const CallSignature* signature = nullptr;
    std::span<const CallSignature::Parameter> parameters;
  };
  [[nodiscard]] std::expected<CallableContract, ExecutionStatus> ResolveCallableContract(
      const ResolvedCallable& callable) const;
  [[nodiscard]] static std::expected<std::size_t, ExecutionStatus> ReadLayoutIndex(
      runtime::Value value,
      std::size_t count);
  StepResult EnterDynamicCall(ActiveFrame& active,
                              const runtime::DynamicCallPlan& plan,
                              const runtime::HeapObject& arguments,
                              const runtime::HeapObject& names,
                              std::uint8_t destination);
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
  [[nodiscard]] bool IsResumedFrame() const {
    return resumed_frame_ != nullptr && frame_count_ == 1;
  }
  StepResult StringOperation(OpCode opcode,
                             runtime::RegisterReference destination,
                             runtime::Value left,
                             runtime::Value right);
  StepResult ArrayOperation(OpCode opcode,
                            runtime::RegisterReference destination,
                            runtime::Value left,
                            runtime::Value right);
  StepResult ExecuteBuiltin(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteTestRegistration(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteTestControl(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteMockInspection(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteMockObservation(MockInspection inspection) const;
  StepResult ExecuteMockCallable(MockInspection inspection);
  [[nodiscard]] bool IsMockResponseCompatible(const FunctionStorage& function,
                                              runtime::Value value) const;
  StepResult ValidateTestFinalizer(runtime::Value value);
  static StepResult ExecuteTypeBinding(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteCalendar(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteJson(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamic(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicCall(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicMember(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicMethodCall(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteDynamicArray(ActiveFrame& active, Instruction instruction);
  StepResult ReadDynamicMember(ActiveFrame& active,
                               Instruction instruction,
                               const MemberDefinition& member);
  StepResult WriteDynamicMember(ActiveFrame& active,
                                Instruction instruction,
                                const MemberDefinition& member);
  [[nodiscard]] std::expected<runtime::Value, ExecutionStatus> CreateDynamicCallPlan(
      runtime::Value value,
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
  [[nodiscard]] runtime::FutureCallbacks MakeFutureCallbacks();
  [[nodiscard]] runtime::FutureCall DescribeFutureCall(
      AsyncOperation operation,
      std::span<const runtime::Value> values) const;
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
  [[clang::always_inline]] StepResult Return(ActiveFrame& active, runtime::Value value);
  [[clang::always_inline]] StepResult ResumeCaller(std::uint8_t return_register,
                                                   runtime::Value value);
  [[nodiscard]] std::vector<StackFrame> CaptureStack() const;
  [[nodiscard]] std::span<const runtime::TracePosition> CapturePositions(
      std::span<runtime::TracePosition> storage) const;
  std::expected<runtime::Value, ExecutionStatus> RuntimeError(ExecutionStatus status,
                                                              std::string_view message);
  StepResult CreateException(runtime::Value payload,
                             ExecutionStatus status,
                             std::string_view message);
  ExecutionStatus RestoreFutureError(const runtime::FutureResult& completion);
  std::optional<ExecutionResult> CompleteAsyncError(ActiveFrame& active);
  [[nodiscard]] ExecutionResult ExceptionResult() const;
  std::optional<ExecutionResult> HandleError(ExecutionStatus status);
  StepResult ExecuteException(ActiveFrame& active, Instruction instruction);
  void ResetInitializer(ActiveFrame& active) noexcept;
  void AbortInitializers() noexcept;

  runtime::Isolate& isolate_;
  const Program& program_;
  std::span<const FunctionStorage> functions_;
  FrameSlots<RegisterFile>& registers_;
  FrameSlots<ActiveFrame> frames_;
  std::size_t frame_count_ = 0;
  runtime::Value result_;
  GlobalStorage* globals_;
  std::string error_message_;
  runtime::Value pending_exception_;
  runtime::RootRange exception_root_{runtime::RootOwner::Runtime};
  runtime::ExternalResource execution_resource_;
  std::shared_ptr<DebugSession> debug_session_;
  SuspendedFrame* resumed_frame_ = nullptr;
  bool resuspended_ = false;
};

}  // namespace vela::vm::internal
