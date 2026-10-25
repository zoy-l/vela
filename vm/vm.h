#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "base/execution_limits.h"
#include "base/source_location.h"
#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/debugger.h"
#include "vm/execution_status.h"
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
  runtime::Value value;
  ExecutionStatus status = ExecutionStatus::InvalidEntry;
  std::vector<StackFrame> stack_trace;
  std::string message;

  [[nodiscard]] auto Succeeded() const noexcept -> bool {
    return status == ExecutionStatus::Returned;
  }
};

void WriteRuntimeError(std::ostream& output, const ExecutionResult& result);

class VirtualMachine final {
 public:
  explicit VirtualMachine(runtime::Isolate& isolate) noexcept : isolate_(isolate) {}

  ~VirtualMachine() = default;
  VirtualMachine(const VirtualMachine&) = delete;
  auto operator=(const VirtualMachine&) -> VirtualMachine& = delete;
  VirtualMachine(VirtualMachine&&) = delete;
  auto operator=(VirtualMachine&&) -> VirtualMachine& = delete;

  [[nodiscard]] auto Execute(const FunctionHandle& function) noexcept -> ExecutionResult;
  [[nodiscard]] auto Execute(const Program& program, std::uint32_t entry_function) noexcept
      -> ExecutionResult;
  [[nodiscard]] auto Debug(const FunctionHandle& function, Debugger& debugger) noexcept
      -> ExecutionResult;
  [[nodiscard]] auto register_bytes() const noexcept -> std::size_t;

 private:
  [[clang::always_inline]] auto ExecuteImpl(const Program& program,
                                            std::uint32_t entry_function,
                                            Debugger* debugger) noexcept -> ExecutionResult;
  runtime::Isolate& isolate_;
  std::array<RegisterFile, kMaxCallFrames> registers_;
  std::vector<runtime::Value> native_arguments_;
};

}  // namespace vela::vm
