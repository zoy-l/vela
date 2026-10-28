#pragma once

namespace vela {

// 工具错误使用 sysexits；脚本显式 int 返回值仍可使用完整的 0..255。
inline constexpr int kUsageExitCode = 64;
inline constexpr int kSourceErrorExitCode = 65;
inline constexpr int kRuntimeErrorExitCode = 70;
inline constexpr int kIoErrorExitCode = 74;
inline constexpr int kMaximumExitCode = 255;

}  // namespace vela
