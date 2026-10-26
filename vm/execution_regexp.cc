#include "runtime/object.h"
#include "vm/execution.h"

#include <array>
#include <cstddef>
#include <cstdint>
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
    if (!index.IsInteger() || index.AsInteger() < 0 ||
        static_cast<std::uint64_t>(index.AsInteger()) >= arrays.size()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    layout = arrays.subspan(static_cast<std::size_t>(index.AsInteger())).front();
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
  if (!result) {
    error_message_ = result.error().message;
    return std::unexpected(result.error().status);
  }
  registers.subspan(DecodeA(instruction)).front() = *result;
  return {};
}

}  // namespace vela::vm::internal
