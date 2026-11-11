#pragma once

#include <iosfwd>
#include <optional>
#include <span>
#include <string_view>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/isolate/external_resources.h"

namespace vela::tooling {

void WriteExternalResources(std::ostream& output,
                            std::span<const runtime::ExternalResourceInfo> resources);
void WriteExecutionFailure(std::ostream& output,
                           const std::optional<runtime::FutureDiagnostic>& diagnostic,
                           std::optional<ExecutionStatus> execution,
                           std::string_view prefix);

}  // namespace vela::tooling
