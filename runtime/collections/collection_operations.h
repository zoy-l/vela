#pragma once

#include <expected>
#include <span>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/value/value.h"

namespace vela::runtime {

[[nodiscard]] std::expected<Value, ExecutionStatus>
ExecuteCollectionBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap);

}  // namespace vela::runtime
