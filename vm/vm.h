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
  IntegerOverflow,
  DivisionByZero,
  StackOverflow,
};

struct ExecutionResult final {
  runtime::Value value;
  ExecutionStatus status = ExecutionStatus::InvalidBytecode;

  [[nodiscard]] bool Succeeded() const noexcept { return status == ExecutionStatus::Returned; }
};

class VirtualMachine final {
 public:
  explicit VirtualMachine(runtime::Isolate& isolate) noexcept : isolate_(isolate) {}

  [[nodiscard]] ExecutionResult Execute(const Function& function) noexcept;
  [[nodiscard]] ExecutionResult Execute(const Program& program,
                                        std::uint32_t entry_function) noexcept;

 private:
  [[nodiscard]] static bool Validate(const Function& function,
                                     std::span<const Function> functions) noexcept;

  runtime::Isolate& isolate_;
};

}  // namespace vela::vm
