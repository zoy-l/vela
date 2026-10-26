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
#include "base/execution_limits.h"
#include "runtime/exception.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
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
            std::vector<runtime::Value>& native_arguments,
            std::shared_ptr<DebugSession> debug_session = {})
      : isolate_(isolate),
        program_(program),
        functions_(program.functions()),
        registers_(registers),
        globals_(program.globals()),
        native_arguments_(&native_arguments),
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
  StepResult ExecuteCalendar(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteRegExp(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteAsync(ActiveFrame& active, Instruction instruction);
  StepResult ExecuteCompleter(const AsyncArguments& call);
  StepResult ExecuteAsyncState(const AsyncArguments& call);
  StepResult ExecuteAsyncComputation(const AsyncArguments& call);
  StepResult ExecuteAsyncAggregation(ActiveFrame& active, const AsyncArguments& call);
  StepResult ExecuteAsyncComposition(ActiveFrame& active, const AsyncArguments& call);
  StepResult ExecuteNative(ActiveFrame& active, Instruction instruction);
  StepResult Interpolate(ActiveFrame& active, Instruction instruction);
  static void Jump(ActiveFrame& active, std::int16_t offset) noexcept;
  [[clang::always_inline]] StepResult Call(ActiveFrame& active, Instruction instruction);
  StepResult CallIndirect(ActiveFrame& active, Instruction instruction);
  StepResult CallInterface(ActiveFrame& active, Instruction instruction);
  [[nodiscard]] std::expected<std::uint32_t, ExecutionStatus> InterfaceTarget(
      runtime::Value receiver,
      std::uint32_t selector) const;
  [[clang::always_inline]] StepResult EnterCall(std::uint32_t target,
                                                ActiveFrame& active,
                                                Instruction instruction,
                                                std::optional<runtime::Value> implicit);
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
  std::vector<runtime::Value>* native_arguments_;
  std::shared_ptr<DebugSession> debug_session_;
};

}  // namespace vela::vm::internal
