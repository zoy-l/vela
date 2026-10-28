#include "vm/vm.h"

#include <ostream>
#include "runtime/process_entry.h"
#include "vm/execution.h"

namespace vela::vm {

void WriteRuntimeError(std::ostream& output, const ExecutionResult& result) {
  runtime::WriteRuntimeDiagnostic(output, internal::Execution::Diagnostic(result));
}

}  // namespace vela::vm
