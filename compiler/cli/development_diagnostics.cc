#include "compiler/cli/development_diagnostics.h"

#include <optional>
#include <ostream>
#include <span>
#include <string_view>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/process/process_entry.h"

namespace vela::tooling {

void WriteExternalResources(std::ostream& output,
                            std::span<const runtime::ExternalResourceInfo> resources) {
  for (const auto& resource : resources) {
    output << "  " << runtime::ExternalResourceKindName(resource.kind) << " #" << resource.identity
           << ": " << resource.references << " reference(s)\n";
  }
}

void WriteExecutionFailure(std::ostream& output,
                           const std::optional<runtime::FutureDiagnostic>& diagnostic,
                           std::optional<ExecutionStatus> execution,
                           std::string_view prefix) {
  if (diagnostic) {
    runtime::WriteRuntimeDiagnostic(output, *diagnostic);
  } else if (execution) {
    output << prefix << ExecutionStatusName(*execution) << '\n';
  }
}

}  // namespace vela::tooling
