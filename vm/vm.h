#pragma once

#include <cstdint>
#include <span>

#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/bytecode.h"

namespace vela::vm {

enum class ExecutionStatus : std::uint8_t {
  Returned,
  InvalidBytecode,
  OutOfMemory,
  TypeError,
  InvalidField,
  IndexOutOfBounds,
  IntegerOverflow,
  DivisionByZero,
  StackOverflow,
};

struct ExecutionResult final {
  runtime::Value value;
  ExecutionStatus status = ExecutionStatus::InvalidBytecode;

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

  [[nodiscard]] auto Execute(const Function& function) noexcept -> ExecutionResult;
  [[nodiscard]] auto Execute(const Program& program, std::uint32_t entry_function) noexcept
      -> ExecutionResult;

 private:
  [[nodiscard]] static auto Validate(const Function& function,
                                     std::span<const Function> functions) noexcept -> bool;

  runtime::Isolate& isolate_;
};

}  // namespace vela::vm
