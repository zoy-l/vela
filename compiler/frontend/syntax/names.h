#pragma once

#include <string_view>

namespace vela::frontend {

inline constexpr std::string_view kConstructorName = "$constructor";
inline constexpr std::string_view kPrivateConstructorName = "#constructor";
inline constexpr char kPrivateNamePrefix = '#';
inline constexpr std::string_view kReceiverName = "this";
inline constexpr std::string_view kSuperName = "super";

[[nodiscard]] constexpr bool IsPrivateName(std::string_view name) noexcept {
  return name.starts_with(kPrivateNamePrefix);
}

[[nodiscard]] bool IsIdentifier(std::string_view text);
[[nodiscard]] bool IsDeclarationIdentifier(std::string_view text);
[[nodiscard]] bool IsMemberIdentifier(std::string_view text);
[[nodiscard]] bool IsNativeTypeKey(std::string_view text);

}  // namespace vela::frontend
