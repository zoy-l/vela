#include "runtime/object.h"
#include "vm/execution.h"

#include <array>
#include <expected>
#include <span>

#include "base/builtin.h"
#include "base/regexp.h"
#include "runtime/isolate.h"
#include "runtime/regexp.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"

namespace vela::vm::internal {

StepResult Execution::ExecuteRegExp(ActiveFrame& active, Instruction instruction) {
  const auto builtin = static_cast<Builtin>(DecodeB(instruction));
  auto registers = active.frame.registers();
  const auto count = BuiltinArity(builtin);
  std::array<runtime::Value, 4> values;
  const auto arguments = std::span(values).first(count);
  registers.subspan(DecodeC(instruction), count).CopyTo(arguments);
  const runtime::ArrayLayout* layout = nullptr;
  if (HasRegExpArrayLayout(builtin)) {
    const auto& tables = active.function->layouts;
    if (tables == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto index = arguments.back();
    const auto arrays = tables->arrays();
    const auto offset = ReadLayoutIndex(index, arrays.size());
    if (!offset) {
      return std::unexpected(offset.error());
    }
    layout = arrays.subspan(*offset).front();
    const auto expected_kind = builtin == Builtin::RegExpAllMatches ? runtime::ValueKind::Object
                                                                    : runtime::ValueKind::String;
    if (layout->element.kind != expected_kind ||
        (expected_kind == runtime::ValueKind::Object &&
         layout->element.object_kind != runtime::ObjectKind::RegExpMatch) ||
        layout->element.nullable != (builtin == Builtin::RegExpMatchGroups)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  const auto result = runtime::ExecuteRegExpBuiltin(builtin, arguments, isolate_.heap(), layout);
  return StoreDiagnosticResult(registers.subspan(DecodeA(instruction)).front(), result);
}

}  // namespace vela::vm::internal
