#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

enum class NativeDebugging : std::uint8_t { Safepoints, SourceLines, Instructions };
inline constexpr std::uint8_t kNativeSafepointBoundary = 1U << 0;
inline constexpr std::uint8_t kNativeSafepointBeforeRuntime = 1U << 1;
inline constexpr std::uint8_t kNativeSafepointAfterRuntime = 1U << 2;
// 仅供假设失败时恢复，不要求在成功路径轮询。
inline constexpr std::uint8_t kNativeSafepointRecovery = 1U << 3;

struct NativeRootPlan final {
  bool debug_values = false;
  bool bounded_polls = false;
  std::size_t count = 0;
  // 槽位按包含异常边和清空点的保守活跃区间复用，不是寄存器的唯一身份。
  std::vector<std::optional<std::size_t>> slots;
  std::vector<std::vector<ir::RegisterId>> stores;
  std::vector<std::vector<ir::RegisterId>> clears;
  std::vector<std::vector<ir::RegisterId>> suspended;
  std::vector<std::vector<std::size_t>> rematerialized;
  std::vector<std::uint8_t> safepoints;
  // 原版本基线恢复使用的规范寄存器编号，包含标量局部值。
  std::vector<std::vector<ir::RegisterId>> checkpoints;
  std::size_t minimum_temporary_count = 0;
};

struct NativeRecoveryRequirements final {
  std::span<const std::vector<ir::RegisterId>> values;
  std::span<const std::uint8_t> safepoints;
  // 物化会绕过 IR 定义写入真实对象；其初始 null 及复制链不能证明可省略帧根。
  std::span<const ir::RegisterId> materialized;
  std::size_t minimum_root_count = 0;
  std::size_t minimum_temporary_count = 0;
  // 借用字段在每个指令入口都保持活动；共享一行，避免按指令数复制相同需求。
  std::span<const ir::RegisterId> uniform_values;
};

struct NativeFunctionMetrics final {
  // 计数描述通用入口的静态计划，不把多个互斥函数体的计数相加。
  // frame_bytes 是同一函数全部生成体的最大值，供递归物理栈预算使用。
  std::uint64_t shadow_slots = 0;
  std::uint64_t frame_bytes = 0;
  std::uint64_t root_stores = 0;
  std::uint64_t eliminated_allocations = 0;
  std::uint64_t sunk_allocations = 0;
  std::uint64_t retained_allocations = 0;
  std::uint64_t stack_records = 0;
  std::uint64_t borrowed_frame_bytes = 0;
  std::uint64_t borrowed_shadow_slots = 0;
  std::uint64_t borrowed_root_stores = 0;
};

// readonly_globals 借用封闭链接程序的全局分析事实；仅同步 Safepoints 入口使用。
// 常量池和只读全局已由实例持有根，其仅含 Move/null 的派生值无需重复的帧根。
[[nodiscard]] NativeRootPlan PlanNativeRoots(const ir::Function& function,
                                             std::span<const ir::Function> functions,
                                             NativeProgramMode mode,
                                             NativeDebugging debugging,
                                             NativeRecoveryRequirements recovery = {},
                                             std::span<const std::uint8_t> readonly_globals = {});

}  // namespace vela::compiler::native
