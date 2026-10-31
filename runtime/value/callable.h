#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "base/execution_status.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

struct ResolvedCallable final {
  std::uint32_t function = 0;
  std::optional<Value> implicit = std::nullopt;
};

[[nodiscard]] std::expected<std::uint32_t, ExecutionStatus>
ResolveInterfaceTarget(std::uint32_t selector, Value receiver, std::size_t function_count);
[[nodiscard]] std::expected<ResolvedCallable, ExecutionStatus>
ResolveCallable(Value value, const CodeIdentity& identity, std::size_t function_count);

}  // namespace vela::runtime
