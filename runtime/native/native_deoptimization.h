#pragma once

#include <cstdint>

#include "base/native_program_mode.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/value.h"

namespace vela::runtime {

struct NativeResumeState;
struct NativeCheckpoint;
struct NativeVirtualGraph;
using NativeRecoveryEntry = std::uint8_t (*)(NativeExecution*, NativeCheckpoint*, void*) noexcept;

enum class NativeCheckpointPhase : std::uint8_t { Instruction, AwaitingResult };

// 随代码版本保活的只读描述；调用帧不重复写入恢复入口和各类槽数量。
struct NativeCheckpointLayout final {
  NativeRecoveryEntry recovery = nullptr;
  std::uint64_t root_count = 0;
  std::uint64_t value_count = 0;
  std::uint64_t reference_count = 0;
};

// 仅含虚拟对象的优化帧需要此状态；graph 随规范指令改变，其余数据随调用固定。
struct NativeVirtualState final {
  const NativeVirtualGraph* graph = nullptr;
  HeapObject*** references = nullptr;
  HeapObject* program = nullptr;
};

// 生成代码持有此存储直到物理帧返回，只物化 instruction 处的活跃值。
// 恢复直接将这些槽用作基线局部存储；精确托管根仍位于该帧的影子槽。
// AwaitingResult 表示基线续延正在等待已进入的调用完成。
// 链接后 tier、phase 与 materialization 始终有效；无物化状态时，其余字段和槽值只在本帧
// 调用 vela_native_v4_recover 前写入，其他帧的安全点不得读取它们。
// 无物化状态的非空标量槽只初始化 payload；类型化恢复不读取其标签。
struct NativeCheckpoint final {
  NativeValue* values = nullptr;
  Value* roots = nullptr;
  void* storage = nullptr;
  NativeResumeState* resume = nullptr;
  const NativeCheckpointLayout* layout = nullptr;
  std::uint32_t instruction = 0;
  NativeCodeTier tier = NativeCodeTier::Optimized;
  NativeCheckpointPhase phase = NativeCheckpointPhase::Instruction;
  NativeVirtualState* materialization = nullptr;
};

extern "C" std::uint8_t vela_native_v4_can_optimize(const NativeExecution* execution,
                                                    const NativeImage* image) noexcept;
extern "C" std::uint8_t vela_native_v4_recover(NativeExecution* execution,
                                               NativeCheckpoint* checkpoint,
                                               void* result) noexcept;

}  // namespace vela::runtime
