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
enum class NativeConstantRoots : std::uint8_t { Local, Instance };

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
  std::size_t minimum_root_count = 0;
  std::size_t minimum_temporary_count = 0;
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

[[nodiscard]] NativeRootPlan PlanNativeRoots(
    const ir::Function& function,
    std::span<const ir::Function> functions,
    NativeProgramMode mode,
    NativeDebugging debugging,
    NativeRecoveryRequirements recovery = {},
    NativeConstantRoots constants = NativeConstantRoots::Local);

}  // namespace vela::compiler::native
