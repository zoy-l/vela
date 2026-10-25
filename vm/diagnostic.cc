#include "vm/vm.h"

#include <ostream>
#include <string_view>

#include "base/diagnostic.h"
#include "base/source_location.h"
#include "vm/execution_status.h"

namespace vela::vm {

void WriteRuntimeError(std::ostream& output, const ExecutionResult& result) {
  const auto& trace = result.stack_trace;
  const auto message = result.message.empty() ? ExecutionStatusName(result.status)
                                              : std::string_view(result.message);
  if (trace.empty()) {
    WriteError(output, {}, SourceLocation(), message);
  } else {
    const auto& origin = trace.front();
    WriteError(output, origin.source_path, origin.location, message);
  }
  for (const auto& frame : trace) {
    output << "  at " << (frame.function.empty() ? "<anonymous>" : frame.function);
    if (!frame.source_path.empty()) {
      output << " (" << frame.source_path << ':' << frame.location.line << ':'
             << frame.location.column << ')';
    } else {
      output << " (instruction " << frame.instruction << ')';
    }
    output << '\n';
  }
}

}  // namespace vela::vm
