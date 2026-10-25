#pragma once

#include <optional>
#include <string_view>

namespace vela::frontend {

inline constexpr std::string_view kErrorModuleName = "std:error";

[[nodiscard]] auto StandardModuleSource(std::string_view name) noexcept
    -> std::optional<std::string_view>;

}  // namespace vela::frontend
