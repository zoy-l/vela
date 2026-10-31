#include "vm/diagnostic.h"

#include <ostream>
#include <span>
#include <vector>

#include "runtime/future.h"
#include "runtime/process_entry.h"
#include "vm/debug_info.h"
#include "vm/execution.h"
#include "vm/vm.h"

namespace vela::vm {

std::vector<runtime::FutureTraceFrame> CopyFutureTrace(std::span<const StackFrame> trace) {
  std::vector<runtime::FutureTraceFrame> result;
  result.reserve(trace.size());
  for (const auto& frame : trace) {
    result.push_back({
        .function = frame.function,
        .source_path = frame.source_path,
        .line = frame.location.line,
        .column = frame.location.column,
        .instruction = frame.instruction,
        .identity = {},
    });
  }
  return result;
}

std::vector<StackFrame> CopyExecutionTrace(std::span<const runtime::FutureTraceFrame> trace) {
  std::vector<StackFrame> result;
  result.reserve(trace.size());
  for (const auto& frame : trace) {
    result.push_back({
        .function = frame.function,
        .source_path = frame.source_path,
        .location = {.line = frame.line, .column = frame.column},
        .instruction = frame.instruction,
    });
  }
  return result;
}

ExecutionResult FutureFailure(const runtime::FutureResult& completion) {
  const auto& diagnostic = completion.diagnostic;
  return {
      .value = completion.value,
      .status = diagnostic.status,
      .stack_trace = CopyExecutionTrace(diagnostic.stack_trace),
      .message = diagnostic.message,
      .origin = diagnostic.origin,
  };
}

void WriteRuntimeError(std::ostream& output, const ExecutionResult& result) {
  runtime::WriteRuntimeDiagnostic(output, internal::Execution::Diagnostic(result));
}

}  // namespace vela::vm
