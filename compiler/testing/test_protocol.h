#pragma once

#include <string_view>

namespace vela::test_fields {

inline constexpr std::string_view kPath = "path";
inline constexpr std::string_view kActual = "actual";
inline constexpr std::string_view kChildren = "children";
inline constexpr std::string_view kDifference = "difference";
inline constexpr std::string_view kMessage = "message";
inline constexpr std::string_view kFailureType = "TestFailure";
inline constexpr std::string_view kDifferenceType = "MatchDifference";

}  // namespace vela::test_fields
