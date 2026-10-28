#pragma once

#include <expected>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/value.h"

namespace vela::runtime {

class NativeExecution;

[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteNativeJson(NativeExecution& execution,
                                                                      Builtin builtin,
                                                                      Value input);

}  // namespace vela::runtime
