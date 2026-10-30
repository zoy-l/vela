#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

#include "base/source_location.h"
#include "runtime/execution_context.h"
#include "runtime/future.h"
#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/debugger.h"
#include "vm/execution_status.h"
#include "vm/frame_slots.h"
#include "vm/program.h"
#include "vm/register_file.h"

namespace vela::vm {

// 错误帧拥有文本，不借用 Program；程序销毁后仍可输出诊断。
struct StackFrame final {
  std::string function;
  std::string source_path;
  SourceLocation location;
  std::size_t instruction = 0;
};

struct ExecutionResult final {
  // 返回引用不自动保根；宿主须在下一次分配或 GC 前注册 RootRange。
  runtime::Value value = runtime::Value::Null();
  ExecutionStatus status = ExecutionStatus::InvalidEntry;
  std::vector<StackFrame> stack_trace;
  std::string message;
  runtime::ErrorOrigin origin;

  [[nodiscard]] bool Succeeded() const noexcept { return status == ExecutionStatus::Returned; }
};

void WriteRuntimeError(std::ostream& output, const ExecutionResult& result);

class VirtualMachine final {
 public:
  explicit VirtualMachine(runtime::Isolate& isolate) noexcept : isolate_(isolate) {}

  ~VirtualMachine() = default;
  VirtualMachine(const VirtualMachine&) = delete;
  VirtualMachine& operator=(const VirtualMachine&) = delete;
  VirtualMachine(VirtualMachine&&) = delete;
  VirtualMachine& operator=(VirtualMachine&&) = delete;

  [[nodiscard]] ExecutionResult Execute(const FunctionHandle& function) noexcept;
  [[nodiscard]] ExecutionResult Execute(const Program& program,
                                        std::uint32_t entry_function) noexcept;
  [[nodiscard]] ExecutionResult Invoke(const Program& program,
                                       runtime::Value callable,
                                       std::span<const runtime::Value> arguments = {}) noexcept;
  [[nodiscard]] runtime::Future ExecuteFuture(const Program& program,
                                              std::uint32_t entry_function) noexcept;
  [[nodiscard]] ExecutionResult Debug(const FunctionHandle& function, Debugger& debugger) noexcept;
  [[nodiscard]] std::size_t register_bytes() const noexcept;

 private:
  [[clang::always_inline]] ExecutionResult ExecuteImpl(const Program& program,
                                                       std::uint32_t entry_function,
                                                       Debugger* debugger) noexcept;
  runtime::Isolate& isolate_;
  FrameSlots<RegisterFile> registers_;
  bool executing_ = false;
};

}  // namespace vela::vm
