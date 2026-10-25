#pragma once

#include <expected>
#include <string_view>

#include "runtime/value.h"

namespace vela::compiler {

// 遵循 sysexits 的工具错误码；脚本显式 int 返回值仍可使用完整的 0..255。
inline constexpr int kUsageExitCode = 64;
inline constexpr int kSourceErrorExitCode = 65;
inline constexpr int kRuntimeErrorExitCode = 70;
inline constexpr int kIoErrorExitCode = 74;
inline constexpr int kMaximumExitCode = 255;

[[nodiscard]] auto ProcessExitCode(runtime::Value value) noexcept
    -> std::expected<int, std::string_view>;

}  // namespace vela::compiler
