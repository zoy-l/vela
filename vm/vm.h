#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "base/execution_limits.h"
#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/program.h"
#include "vm/register_file.h"

namespace vela::vm {

enum class ExecutionStatus : std::uint8_t {
  Returned,
  OutOfMemory,
  TypeError,
  InvalidField,
  IndexOutOfBounds,
  IntegerOverflow,
  DivisionByZero,
  StackOverflow,
  InvalidEntry,
  InvalidIsolate,
};

struct ExecutionResult final {
  // 返回引用不自动保根；宿主须在下一次分配或 GC 前注册 RootRange。
  runtime::Value value;
  ExecutionStatus status = ExecutionStatus::InvalidEntry;

  [[nodiscard]] auto Succeeded() const noexcept -> bool {
    return status == ExecutionStatus::Returned;
  }
};

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
  [[nodiscard]] auto register_bytes() const noexcept -> std::size_t;

 private:
  runtime::Isolate& isolate_;
  std::array<RegisterFile, kMaxCallFrames> registers_;
};

}  // namespace vela::vm
