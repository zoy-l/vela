#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/execution_limits.h"
#include "runtime/exception.h"
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

struct ActiveFrame final {
  Frame frame;
  const FunctionStorage* function = nullptr;
  std::size_t program_counter = 0;
  std::uint8_t return_register = 0;
  std::optional<std::size_t> initialized_global;
};

class Execution final {
 public:
  Execution(runtime::Isolate& isolate,
            const Program& program,
            std::span<RegisterFile> registers,
            std::vector<runtime::Value>& native_arguments)
      : isolate_(isolate),
        functions_(program.functions()),
        registers_(registers),
        globals_(program.globals()),
        native_arguments_(&native_arguments) {}

  ExecutionResult Run(std::uint32_t entry_function) noexcept;
  ExecutionResult DebugRun(std::uint32_t entry_function, Debugger& debugger) noexcept;

 private:
  template <bool Debugging>
  ExecutionResult RunImpl(std::uint32_t entry_function, Debugger* debugger) noexcept;
  // 保持热分派与调用/返回在同一优化边界；Windows ABI 的寄存器视图实参传递曾导致明显回归。
  [[clang::always_inline]] StepResult ExecuteInstruction(ActiveFrame& active,
                                                         Instruction instruction);
  [[clang::always_inline]] StepResult ExecuteControl(ActiveFrame& active, Instruction instruction);
  [[clang::always_inline]] StepResult ExecuteObject(ActiveFrame& active, Instruction instruction);
  StepResult AllocateObject(ActiveFrame& active, Instruction instruction);
  StepResult StringOperation(OpCode opcode,
                             runtime::RegisterReference destination,
                             runtime::Value left,
                             runtime::Value right);
  StepResult ArrayOperation(OpCode opcode,
                            runtime::RegisterReference destination,
                            runtime::Value left,
                            runtime::Value right);
  StepResult ExecuteBuiltin(ActiveFrame& active, Instruction instruction);
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
  [[clang::always_inline]] void Return(ActiveFrame& active, runtime::Value value);
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
  std::optional<ExecutionResult> HandleError(ExecutionStatus status);
  StepResult ExecuteException(ActiveFrame& active, Instruction instruction);
  void AbortInitializers() noexcept;

  runtime::Isolate& isolate_;
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
};

}  // namespace vela::vm::internal
