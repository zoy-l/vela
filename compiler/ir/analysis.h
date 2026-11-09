#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/member.h"
#include "base/opcode.h"
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
  // 挂起当前激活帧；创建 Future 或注册回调本身不挂起调用者。
  bool may_suspend = false;
  bool reads_state = false;
  bool writes_state = false;
  bool changes_control = false;

  [[nodiscard]] bool MayCollectOnSuccess() const noexcept { return may_allocate; }
  [[nodiscard]] bool MayCollect() const noexcept { return may_fail || MayCollectOnSuccess(); }
  [[nodiscard]] bool IsRemovable() const noexcept {
    return !MayCollect() && !writes_state && !changes_control;
  }
};

// 普通控制流分析不跨宿主实现、异步挂起或异常处理边。
[[nodiscard]] bool CanAnalyzeSynchronousFunction(const Function& function);

[[nodiscard]] inline bool IsBackJump(const Instruction& instruction, std::size_t index) {
  return IsJumpOpcode(instruction.opcode) && instruction.index <= index;
}

[[nodiscard]] inline bool IsBuiltinCall(const Instruction& instruction, Builtin builtin) {
  return instruction.opcode == Opcode::BuiltinCall &&
         instruction.index == static_cast<std::uint32_t>(builtin);
}

enum class ClassFunctionKind : std::uint8_t { Constructor, Entry, FieldInitializer };

// 只枚举实际函数编号；接口 callable 的 function 字段是 selector，不是函数目标。
template <typename Visitor>
void ForEachClassFunction(const Class& type, const Visitor& visitor) {
  const auto visit = [&](std::uint32_t target, ClassFunctionKind kind) {
    if (target != kInvalidFunctionTarget) {
      visitor(target, kind);
    }
  };
  visit(type.constructor, ClassFunctionKind::Constructor);
  if (type.callable && type.callable->mode != CallableMode::InterfaceMethod) {
    visit(type.callable->function, ClassFunctionKind::Entry);
  }
  for (const auto target : type.dispatch_targets) {
    visit(target, ClassFunctionKind::Entry);
  }
  for (const auto& field : type.fields) {
    visit(field.initializer, ClassFunctionKind::FieldInitializer);
  }
}

template <typename Visitor>
void ForEachPropertyFunction(const MemberDefinition& member, const Visitor& visitor) {
  if (member.kind != MemberKind::Property) {
    return;
  }
  if (member.target != kInvalidFunctionTarget) {
    visitor(member.target);
  }
  if (member.setter) {
    visitor(*member.setter);
  }
}

[[nodiscard]] InstructionEffects AnalyzeEffects(const Instruction& instruction);
void RemapJumpTargets(Function& function, std::span<const std::uint32_t> targets);

[[nodiscard]] RegisterAccess AnalyzeRegisters(const Instruction& instruction,
                                              std::span<const Function> functions);
// 直接 Call 的实参数量属于目标函数；这里只读取指令中编码的调用窗口。
[[nodiscard]] std::uint32_t CallArgumentCount(const Instruction& instruction);
[[nodiscard]] std::vector<RegisterAccess> AnalyzeRegisterAccesses(
    const Function& function,
    std::span<const Function> functions);

struct RegisterDefinition final {
  std::size_t count = 0;
  std::optional<std::size_t> first;
  std::optional<std::size_t> last;
};

// 参数自带一个入口定义；first/last 只记录指令定义，避免把覆写参数误作单定义值。
[[nodiscard]] std::vector<RegisterDefinition> FindRegisterDefinitions(
    const Function& function,
    std::span<const RegisterAccess> accesses);
struct RegisterFlow final {
  std::vector<RegisterDefinition> definitions;
  // 单定义 Move 链归并到同一来源；覆写和无入口来源的环不产生 origin。
  std::vector<std::optional<RegisterId>> origins;

  [[nodiscard]] std::optional<std::size_t> OriginDefinition(RegisterId reg) const;
};
[[nodiscard]] RegisterFlow AnalyzeRegisterFlow(const Function& function,
                                               std::span<const RegisterAccess> accesses);
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

[[nodiscard]] FlowGraph BuildFlowGraph(const Function& function);
[[nodiscard]] std::vector<std::uint8_t> FindReachableBlocks(const FlowGraph& graph);
[[nodiscard]] std::vector<BlockLiveness> AnalyzeLiveness(
    const Function& function,
    const FlowGraph& graph,
    std::span<const Function> functions,
    std::span<const std::vector<RegisterId>> required = {});
[[nodiscard]] std::vector<std::uint8_t> AnalyzeNecessaryLiveness(
    const Function& function,
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
void CompactInstructions(Function& function,
                         std::span<const std::uint8_t> keep,
                         std::vector<std::size_t>* origins = nullptr);

}  // namespace vela::ir
