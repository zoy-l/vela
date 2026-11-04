#pragma once

#include <string_view>

namespace vela::frontend {

inline constexpr std::string_view kConstructorName = "$constructor";
inline constexpr std::string_view kReceiverName = "this";
inline constexpr std::string_view kSuperName = "super";

[[nodiscard]] bool IsIdentifier(std::string_view text);
[[nodiscard]] bool IsDeclarationIdentifier(std::string_view text);
[[nodiscard]] bool IsMemberIdentifier(std::string_view text);
[[nodiscard]] bool IsNativeTypeKey(std::string_view text);

}  // namespace vela::frontend
