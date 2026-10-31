#include "vm/execution.h"

#include <array>
#include <expected>
#include <span>

#include "base/builtin.h"
#include "runtime/isolate/isolate.h"
#include "runtime/library/date.h"
#include "runtime/value/value.h"
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
    const auto offset = ReadLayoutIndex(index, classes.size());
    if (!offset) {
      return std::unexpected(offset.error());
    }
    layout = classes.subspan(*offset).front();
    if (layout != &isolate_.date_parts_layout()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  const auto result = runtime::ExecuteDateBuiltin(builtin, arguments, isolate_.heap(), layout);
  return StoreDiagnosticResult(registers.subspan(DecodeA(instruction)).front(), result);
}

}  // namespace vela::vm::internal
