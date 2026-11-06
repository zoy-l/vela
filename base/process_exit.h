#pragma once

namespace vela {

// 主程序执行失败为 1；工具命令保留各自的 sysexits 错误分类。
inline constexpr int kProgramFailureExitCode = 1;
inline constexpr int kUsageExitCode = 64;
inline constexpr int kSourceErrorExitCode = 65;
inline constexpr int kRuntimeErrorExitCode = 70;
inline constexpr int kIoErrorExitCode = 74;

}  // namespace vela
