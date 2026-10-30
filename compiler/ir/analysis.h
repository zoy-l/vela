#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "compiler/ir/ir.h"
#include "compiler/ir/register_set.h"

namespace vela::ir {

enum class Operand : std::uint8_t { Destination, Left, Right };

struct RegisterAccess final {
  bool defines_value = false;
  std::array<Operand, 3> uses = {};
  std::uint8_t use_count = 0;
  std::uint32_t argument_count = 0;
};

struct InstructionEffects final {
  bool may_fail = false;
  // 成功路径上的托管分配；失败封套的分配由 may_fail 单独覆盖。
  bool may_allocate = false;
  // 可进入脚本或宿主代码，尚未证明无分配的调用必须视为 GC 安全点。
  bool may_call = false;
  // 挂起当前激活帧；创建 Future 或注册回调本身不挂起调用者。
  bool may_suspend = false;
  bool reads_state = false;
  bool writes_state = false;
  bool changes_control = false;

  [[nodiscard]] bool MayCollectOnSuccess() const noexcept {
    return may_allocate || may_call || may_suspend;
  }
  [[nodiscard]] bool MayCollect() const noexcept { return may_fail || MayCollectOnSuccess(); }
  [[nodiscard]] bool IsRemovable() const noexcept {
    return !MayCollect() && !writes_state && !changes_control;
  }
};

[[nodiscard]] InstructionEffects AnalyzeEffects(const Instruction& instruction);

[[nodiscard]] RegisterAccess AnalyzeRegisters(const Instruction& instruction,
                                              std::span<const Function> functions);
[[nodiscard]] RegisterId ReadOperand(const Instruction& instruction, Operand operand);
void WriteOperand(Instruction& instruction, Operand operand, RegisterId value);

template <typename Visitor>
void ForEachUse(const Instruction& instruction,
                const RegisterAccess& access,
                const Visitor& visitor) {
  for (const auto operand : std::span(access.uses).first(access.use_count)) {
    visitor(ReadOperand(instruction, operand));
  }
  for (std::uint32_t index = 0; index < access.argument_count; ++index) {
    visitor(instruction.lhs + index);
  }
}

struct BasicBlock final {
  std::size_t begin = 0;
  std::size_t end = 0;
  std::vector<std::size_t> successors;
  std::vector<std::size_t> predecessors;
};

struct FlowGraph final {
  std::vector<BasicBlock> blocks;
  std::vector<std::size_t> instruction_blocks;
};

struct BlockLiveness final {
  RegisterSet live_in;
  RegisterSet live_out;
};

struct NecessaryLiveness final {
  std::vector<BlockLiveness> blocks;
  std::vector<std::uint8_t> keep;
};

[[nodiscard]] FlowGraph BuildFlowGraph(const Function& function);
[[nodiscard]] std::vector<BlockLiveness> AnalyzeLiveness(
    const Function& function,
    const FlowGraph& graph,
    std::span<const Function> functions,
    std::span<const std::vector<RegisterId>> required = {});
[[nodiscard]] NecessaryLiveness AnalyzeNecessaryLiveness(const Function& function,
                                                         const FlowGraph& graph,
                                                         std::span<const Function> functions);
// 成功边上的定义覆盖旧值，异常边上的定义不生效；两个后端共用此传递规则。
void TransferLiveness(const Function& function,
                      const FlowGraph& graph,
                      std::span<const BlockLiveness> blocks,
                      const Instruction& instruction,
                      const RegisterAccess& access,
                      RegisterSet& live,
                      std::span<const RegisterId> required = {});
void CompactInstructions(Function& function, std::span<const std::uint8_t> keep);

}  // namespace vela::ir
