#include "vm/execution.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "base/builtin.h"
#include "runtime/date.h"
#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"

namespace vela::vm::internal {

StepResult Execution::ExecuteCalendar(ActiveFrame& active, Instruction instruction) {
  const auto builtin = static_cast<Builtin>(DecodeB(instruction));
  auto registers = active.frame.registers();
  const auto count = BuiltinArity(builtin);
  std::array<runtime::Value, runtime::kDatePartsFieldCount> values;
  const auto arguments = std::span(values).first(count);
  registers.subspan(DecodeC(instruction), count).CopyTo(arguments);
  const runtime::ClassLayout* layout = nullptr;
  if (builtin == Builtin::DateToUTC) {
    const auto index = arguments.subspan(1).front();
    const auto classes = active.function->layouts->classes();
    if (!index.IsInteger() || index.AsInteger() < 0 ||
        static_cast<std::uint64_t>(index.AsInteger()) >= classes.size()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    layout = classes.subspan(static_cast<std::size_t>(index.AsInteger())).front();
    if (layout != &isolate_.date_parts_layout()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  const auto result = runtime::ExecuteDateBuiltin(builtin, arguments, isolate_.heap(), layout);
  if (!result) {
    error_message_ = result.error().message;
    return std::unexpected(result.error().status);
  }
  registers.subspan(DecodeA(instruction)).front() = *result;
  return {};
}

}  // namespace vela::vm::internal
