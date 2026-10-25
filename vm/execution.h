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

  auto Run(std::uint32_t entry_function) noexcept -> ExecutionResult;
  auto DebugRun(std::uint32_t entry_function, Debugger& debugger) noexcept -> ExecutionResult;

 private:
  template <bool Debugging>
  auto RunImpl(std::uint32_t entry_function, Debugger* debugger) noexcept -> ExecutionResult;
  // 保持热分派与调用/返回在同一优化边界；Windows ABI 的寄存器视图实参传递曾导致明显回归。
  [[clang::always_inline]] auto ExecuteInstruction(ActiveFrame& active, Instruction instruction)
      -> StepResult;
  [[clang::always_inline]] auto ExecuteControl(ActiveFrame& active, Instruction instruction)
      -> StepResult;
  [[clang::always_inline]] auto ExecuteObject(ActiveFrame& active, Instruction instruction)
      -> StepResult;
  auto AllocateObject(ActiveFrame& active, Instruction instruction) -> StepResult;
  auto StringOperation(OpCode opcode,
                       runtime::RegisterReference destination,
                       runtime::Value left,
                       runtime::Value right) -> StepResult;
  auto ArrayOperation(OpCode opcode,
                      runtime::RegisterReference destination,
                      runtime::Value left,
                      runtime::Value right) -> StepResult;
  auto ExecuteBuiltin(ActiveFrame& active, Instruction instruction) -> StepResult;
  auto ExecuteNative(ActiveFrame& active, Instruction instruction) -> StepResult;
  auto Interpolate(ActiveFrame& active, Instruction instruction) -> StepResult;
  static void Jump(ActiveFrame& active, std::int16_t offset) noexcept;
  [[clang::always_inline]] auto Call(ActiveFrame& active, Instruction instruction) -> StepResult;
  auto CallIndirect(ActiveFrame& active, Instruction instruction) -> StepResult;
  auto CallInterface(ActiveFrame& active, Instruction instruction) -> StepResult;
  [[nodiscard]] auto InterfaceTarget(runtime::Value receiver, std::uint32_t selector) const
      -> std::expected<std::uint32_t, ExecutionStatus>;
  [[clang::always_inline]] auto EnterCall(std::uint32_t target,
                                          ActiveFrame& active,
                                          Instruction instruction,
                                          std::optional<runtime::Value> implicit) -> StepResult;
  auto AccessGlobal(ActiveFrame& active, Instruction instruction) -> StepResult;
  auto ExecuteExpected(ActiveFrame& active, Instruction instruction) -> StepResult;
  [[clang::always_inline]] void Return(ActiveFrame& active, runtime::Value value);
  [[nodiscard]] auto CaptureStack() const -> std::vector<StackFrame>;
  [[nodiscard]] auto ResolveTrace(std::span<const runtime::TracePosition> positions) const
      -> std::vector<StackFrame>;
  [[nodiscard]] auto CapturePositions(std::span<runtime::TracePosition> storage) const
      -> std::span<const runtime::TracePosition>;
  auto RuntimeError(ExecutionStatus status, std::string_view message)
      -> std::expected<runtime::Value, ExecutionStatus>;
  auto CreateException(runtime::Value payload, ExecutionStatus status, std::string_view message)
      -> StepResult;
  auto HandleError(ExecutionStatus status) -> std::optional<ExecutionResult>;
  auto ExecuteException(ActiveFrame& active, Instruction instruction) -> StepResult;
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
