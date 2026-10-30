#pragma once

#include <cstdint>

#include "base/native_program_mode.h"
#include "runtime/native_abi.h"
#include "runtime/value.h"

namespace vela::runtime {

struct NativeResumeState;
struct NativeCheckpoint;
using NativeRecoveryEntry = std::uint8_t (*)(NativeExecution*, NativeCheckpoint*, void*) noexcept;

enum class NativeCheckpointPhase : std::uint8_t { Instruction, AwaitingResult };

// 生成代码持有此存储直到物理帧返回，只物化 instruction 处的活跃值。
// 恢复直接将这些槽用作基线局部存储；精确托管根仍位于该帧的影子槽。
// AwaitingResult 表示基线续延正在等待已进入的调用完成。
struct NativeCheckpoint final {
  NativeValue* values = nullptr;
  Value* roots = nullptr;
  void* storage = nullptr;
  NativeResumeState* resume = nullptr;
  NativeRecoveryEntry recovery = nullptr;
  std::uint32_t instruction = 0;
  NativeCodeTier tier = NativeCodeTier::Optimized;
  NativeCheckpointPhase phase = NativeCheckpointPhase::Instruction;
};

[[nodiscard]] bool CanUseNativeOptimization(const NativeExecution& execution,
                                            const NativeImage& image) noexcept;

extern "C" std::uint8_t vela_native_v4_can_optimize(const NativeExecution* execution,
                                                    const NativeImage* image) noexcept;
extern "C" std::uint8_t vela_native_v4_recover(NativeExecution* execution,
                                               NativeCheckpoint* checkpoint,
                                               void* result) noexcept;

}  // namespace vela::runtime
