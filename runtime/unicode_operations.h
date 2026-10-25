#pragma once

#include <expected>
#include <span>

#include "base/builtin.h"
#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::runtime {

[[nodiscard]] auto ExecuteUnicodeBuiltin(Builtin builtin,
                                         std::span<const Value> arguments,
                                         Heap& heap) -> std::expected<Value, BuiltinError>;

}  // namespace vela::runtime
