#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace vela::frontend {

// Canonical names refer to static storage; other names remain valid for the process lifetime.
[[nodiscard]] std::string_view InternTypeName(std::string_view name);
[[nodiscard]] std::string_view InternTypeName(std::string&& name);

}  // namespace vela::frontend
