#include "compiler/ir/analysis.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "base/opcode.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/register_set.h"

namespace vela::ir {

bool CanAnalyzeSynchronousFunction(const Function& function) {
  return function.native_symbol.empty() && !function.is_async && function.handlers.empty();
}

std::vector<RegisterAccess> AnalyzeRegisterAccesses(const Function& function,
                                                    std::span<const Function> functions) {
  std::vector<RegisterAccess> accesses;
  accesses.reserve(function.instructions.size());
  for (const auto& instruction : function.instructions) {
    accesses.push_back(AnalyzeRegisters(instruction, functions));
  }
  return accesses;
}

std::vector<RegisterDefinition> FindRegisterDefinitions(const Function& function,
                                                        std::span<const RegisterAccess> accesses) {
  std::vector<RegisterDefinition> definitions(function.register_count);
  for (RegisterId reg = 0; reg < function.parameter_count; ++reg) {
    definitions[reg].count = 1;
  }
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    if (!accesses[index].defines_value) {
      continue;
    }
    auto& definition = definitions[function.instructions[index].dst];
    if (!definition.first) {
      definition.first = index;
    }
    definition.last = index;
    ++definition.count;
  }
  return definitions;
}

namespace {

std::vector<std::optional<RegisterId>> FindRegisterOrigins(
    const Function& function,
    std::span<const RegisterDefinition> definitions) {
  std::vector<std::optional<RegisterId>> origins(function.register_count);
  std::vector<std::vector<RegisterId>> copies(function.register_count);
  std::vector<RegisterId> pending;
  for (RegisterId reg = 0; reg < function.register_count; ++reg) {
    const auto& definition = definitions[reg];
    if (definition.count != 1) {
      continue;
    }
    const auto* instruction = definition.last ? &function.instructions[*definition.last] : nullptr;
    if (instruction != nullptr && instruction->opcode == Opcode::Move) {
      copies[instruction->lhs].push_back(reg);
    } else {
      origins[reg] = reg;
      pending.push_back(reg);
    }
  }
  while (!pending.empty()) {
    const auto reg = pending.back();
    pending.pop_back();
    for (const auto copy : copies[reg]) {
      origins[copy] = origins[reg];
      pending.push_back(copy);
    }
  }
  return origins;
}

}  // namespace

RegisterFlow AnalyzeRegisterFlow(const Function& function,
                                 std::span<const RegisterAccess> accesses) {
  auto definitions = FindRegisterDefinitions(function, accesses);
  auto origins = FindRegisterOrigins(function, definitions);
  return {.definitions = std::move(definitions), .origins = std::move(origins)};
}

std::optional<std::size_t> RegisterFlow::OriginDefinition(RegisterId reg) const {
  const auto origin = origins[reg];
  return origin ? definitions[*origin].last : std::nullopt;
}

std::uint32_t CallArgumentCount(const Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::CallIndirect:
      return instruction.index;
    case Opcode::CallInterface:
      return static_cast<std::uint32_t>(instruction.immediate);
    case Opcode::BuiltinCall:
      return static_cast<std::uint32_t>(BuiltinArity(static_cast<Builtin>(instruction.index)));
    case Opcode::AsyncCall:
      return static_cast<std::uint32_t>(AsyncArity(static_cast<AsyncOperation>(instruction.index)));
    case Opcode::StringInterpolate:
      return instruction.rhs;
    default:
      return 0;
  }
}

RegisterAccess AnalyzeRegisters(const Instruction& instruction,
                                std::span<const Function> functions) {
  using Field = Operand;
  switch (instruction.opcode) {
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::LoadInt:
    case Opcode::LoadDouble:
    case Opcode::LoadString:
    case Opcode::NewArray:
    case Opcode::NewCollection:
    case Opcode::NewInstance:
    case Opcode::GetGlobal:
    case Opcode::Catch:
      return {.defines_value = true};
    case Opcode::Move:
    case Opcode::NegInt:
    case Opcode::BitNotInt:
    case Opcode::NegDouble:
    case Opcode::NotBool:
    case Opcode::GetField:
    case Opcode::IsType:
    case Opcode::CheckedCast:
    case Opcode::NewDynamicArray:
    case Opcode::ExceptionValue:
    case Opcode::NewFuture:
    case Opcode::Await:
    case Opcode::SleepFuture:
      return {.defines_value = true, .uses = {Field::Left}, .use_count = 1};
    case Opcode::SetField:
      return {.uses = {Field::Destination, Field::Left}, .use_count = 2};
    case Opcode::SetIndex:
      return {.uses = {Field::Destination, Field::Left, Field::Right}, .use_count = 3};
    case Opcode::Print:
    case Opcode::SetGlobal:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
    case Opcode::Throw:
    case Opcode::Rethrow:
      return {.uses = {Field::Left}, .use_count = 1};
    case Opcode::Return:
      return {.uses = {Field::Destination}, .use_count = 1};
    case Opcode::Jump:
      return {};
    case Opcode::Call:
      return {
          .defines_value = true,
          .argument_count = functions[instruction.index].parameter_count,
      };
    case Opcode::CallIndirect:
      return {
          .defines_value = true,
          .uses = {Field::Right},
          .use_count = 1,
          .argument_count = CallArgumentCount(instruction),
      };
    case Opcode::CallInterface:
    case Opcode::BuiltinCall:
    case Opcode::AsyncCall:
    case Opcode::StringInterpolate:
      return {.defines_value = true, .argument_count = CallArgumentCount(instruction)};
    case Opcode::AddInt:
    case Opcode::SubInt:
    case Opcode::MulInt:
    case Opcode::DivInt:
    case Opcode::ModInt:
    case Opcode::BitAndInt:
    case Opcode::BitOrInt:
    case Opcode::BitXorInt:
    case Opcode::ShiftLeftInt:
    case Opcode::ShiftRightInt:
    case Opcode::ShiftUnsignedRightInt:
    case Opcode::AddDouble:
    case Opcode::SubDouble:
    case Opcode::MulDouble:
    case Opcode::DivDouble:
    case Opcode::EqualDouble:
    case Opcode::NotEqualDouble:
    case Opcode::LessDouble:
    case Opcode::LessEqualDouble:
    case Opcode::GreaterDouble:
    case Opcode::GreaterEqualDouble:
    case Opcode::EqualInt:
    case Opcode::NotEqualInt:
    case Opcode::EqualBool:
    case Opcode::NotEqualBool:
    case Opcode::EqualValue:
    case Opcode::NotEqualValue:
    case Opcode::LessInt:
    case Opcode::LessEqualInt:
    case Opcode::GreaterInt:
    case Opcode::GreaterEqualInt:
    case Opcode::StringConcat:
    case Opcode::EqualString:
    case Opcode::NotEqualString:
    case Opcode::GetIndex:
      return {.defines_value = true, .uses = {Field::Left, Field::Right}, .use_count = 2};
  }
  std::unreachable();
}

RegisterId ReadOperand(const Instruction& instruction, Operand operand) {
  switch (operand) {
    case Operand::Destination:
      return instruction.dst;
    case Operand::Left:
      return instruction.lhs;
    case Operand::Right:
      return instruction.rhs;
  }
  std::unreachable();
}

void WriteOperand(Instruction& instruction, Operand operand, RegisterId value) {
  switch (operand) {
    case Operand::Destination:
      instruction.dst = value;
      return;
    case Operand::Left:
      instruction.lhs = value;
      return;
    case Operand::Right:
      instruction.rhs = value;
      return;
  }
  std::unreachable();
}

namespace {

std::vector<std::uint8_t> FindBlockStarts(const Function& function) {
  const auto code = std::span(function.instructions);
  std::vector<std::uint8_t> starts(code.size(), 0);
  starts.front() = 1;
  for (const auto& handler : function.handlers) {
    if (handler.target < code.size()) {
      starts.at(handler.target) = 1;
    }
  }
  for (std::size_t index = 0; index < code.size(); ++index) {
    const auto& instruction = code[index];
    const auto opcode = instruction.opcode;
    const auto is_jump = IsJumpOpcode(opcode);
    const auto effects = AnalyzeEffects(instruction);
    if (is_jump) {
      if (instruction.index < code.size()) {
        starts.at(instruction.index) = 1;
      }
    }
    if ((is_jump || opcode == Opcode::Return || opcode == Opcode::Throw ||
         opcode == Opcode::Rethrow || effects.may_suspend ||
         (instruction.handler && effects.may_fail)) &&
        index + 1 < code.size()) {
      starts.at(index + 1) = 1;
    }
    if (index != 0 && instruction.handler != code[index - 1].handler) {
      starts.at(index) = 1;
    }
  }
  return starts;
}

}  // namespace

FlowGraph BuildFlowGraph(const Function& function) {
  FlowGraph graph;
  const auto& code = function.instructions;
  if (code.empty()) {
    return graph;
  }
  const auto starts = FindBlockStarts(function);
  graph.instruction_blocks.resize(code.size());
  for (std::size_t index = 0; index < code.size(); ++index) {
    if (starts.at(index) != 0) {
      if (!graph.blocks.empty()) {
        graph.blocks.back().end = index;
      }
      graph.blocks.push_back(
          {.begin = index, .end = code.size(), .successors = {}, .predecessors = {}});
    }
    graph.instruction_blocks.at(index) = graph.blocks.size() - 1;
  }
  for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
    auto& block = graph.blocks.at(index);
    const auto& last = code.at(block.end - 1);
    if (IsJumpOpcode(last.opcode) && last.index < code.size()) {
      block.successors.push_back(graph.instruction_blocks.at(last.index));
    }
    if (last.opcode != Opcode::Jump && last.opcode != Opcode::Return &&
        last.opcode != Opcode::Throw && last.opcode != Opcode::Rethrow &&
        index + 1 < graph.blocks.size()) {
      block.successors.push_back(index + 1);
    }
    if (last.handler && AnalyzeEffects(last).may_fail) {
      const auto target = function.handlers.at(*last.handler).target;
      block.successors.push_back(graph.instruction_blocks.at(target));
    }
    for (const auto successor : block.successors) {
      graph.blocks.at(successor).predecessors.push_back(index);
    }
  }
  return graph;
}

std::vector<std::uint8_t> FindReachableBlocks(const FlowGraph& graph) {
  std::vector<std::uint8_t> reachable(graph.blocks.size());
  if (graph.blocks.empty()) {
    return reachable;
  }
  std::vector<std::size_t> work{0};
  reachable.front() = 1;
  while (!work.empty()) {
    const auto index = work.back();
    work.pop_back();
    for (const auto successor : graph.blocks[index].successors) {
      if (reachable[successor] == 0) {
        reachable[successor] = 1;
        work.push_back(successor);
      }
    }
  }
  return reachable;
}

namespace {

std::optional<std::size_t> ExceptionalBlock(const Function& function,
                                            const FlowGraph& graph,
                                            const Instruction& instruction,
                                            const InstructionEffects& effects) {
  if (!instruction.handler || !effects.may_fail) {
    return std::nullopt;
  }
  return graph.instruction_blocks.at(function.handlers.at(*instruction.handler).target);
}

void TransferInstruction(const Instruction& instruction,
                         const RegisterAccess& access,
                         std::optional<std::size_t> exceptional,
                         std::span<const BlockLiveness> blocks,
                         RegisterSet& live) {
  if (access.defines_value) {
    live.Erase(instruction.dst);
  }
  if (exceptional) {
    live.UnionWith(blocks[*exceptional].live_in);
  }
  ForEachUse(instruction, access, [&](RegisterId reg) { live.Insert(reg); });
}

struct ExceptionalTransfer final {
  std::size_t successor;
  RegisterId destination;
};

struct BlockTransfer final {
  RegisterSet generated;
  RegisterSet killed;
  std::optional<ExceptionalTransfer> exceptional;
};

std::vector<BlockTransfer> SummarizeBlocks(const Function& function,
                                           const FlowGraph& graph,
                                           std::span<const Function> functions,
                                           std::span<const std::vector<RegisterId>> required,
                                           std::span<const RegisterId> uniform_required) {
  std::vector<BlockTransfer> transfers;
  transfers.reserve(graph.blocks.size());
  RegisterSet uniform(function.register_count);
  for (const auto reg : uniform_required) {
    uniform.Insert(reg);
  }
  for (const auto& block : graph.blocks) {
    BlockTransfer transfer{
        .generated = uniform,
        .killed = RegisterSet(function.register_count),
        .exceptional = std::nullopt,
    };
    for (auto position = block.begin; position < block.end; ++position) {
      const auto& instruction = function.instructions.at(position);
      const auto access = AnalyzeRegisters(instruction, functions);
      if (position + 1 == block.end && access.defines_value &&
          !transfer.killed.Has(instruction.dst)) {
        const auto exceptional =
            ExceptionalBlock(function, graph, instruction, AnalyzeEffects(instruction));
        if (exceptional) {
          // CFG 在受查指令之后切块；只有尚未被前序定义覆盖的旧目标值需要补回。
          transfer.exceptional =
              ExceptionalTransfer{.successor = *exceptional, .destination = instruction.dst};
        }
      }
      const auto record_use = [&](RegisterId reg) {
        if (!transfer.killed.Has(reg)) {
          transfer.generated.Insert(reg);
        }
      };
      ForEachUse(instruction, access, record_use);
      const auto extra_uses = required.empty() ? std::span<const RegisterId>{} : required[position];
      for (const auto reg : extra_uses) {
        record_use(reg);
      }
      if (access.defines_value) {
        transfer.killed.Insert(instruction.dst);
      }
    }
    transfers.push_back(std::move(transfer));
  }
  return transfers;
}

enum class LivenessStorage : std::uint8_t { Inputs, InputsAndOutputs };

template <LivenessStorage Storage, typename TransferBlock>
std::vector<BlockLiveness> SolveLiveness(const Function& function,
                                         const FlowGraph& graph,
                                         const TransferBlock& transfer_block) {
  std::vector<BlockLiveness> live;
  live.reserve(graph.blocks.size());
  std::vector<std::size_t> work;
  work.reserve(graph.blocks.size());
  std::vector<std::uint8_t> queued(graph.blocks.size(), 1);
  for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
    live.push_back({
        .live_in = RegisterSet(function.register_count),
        .live_out =
            RegisterSet(Storage == LivenessStorage::InputsAndOutputs ? function.register_count : 0),
    });
    work.push_back(index);
  }
  RegisterSet live_out(function.register_count);
  RegisterSet live_in(function.register_count);
  while (!work.empty()) {
    const auto index = work.back();
    work.pop_back();
    queued.at(index) = 0;
    auto& current = live.at(index);
    live_out.Clear();
    const auto& block = graph.blocks.at(index);
    for (const auto successor : block.successors) {
      const auto& next = live.at(successor).live_in;
      live_out.UnionWith(next);
    }
    live_in = live_out;
    transfer_block(index, live, live_in);
    if constexpr (Storage == LivenessStorage::InputsAndOutputs) {
      current.live_out.Swap(live_out);
    }
    if (current.live_in == live_in) {
      continue;
    }
    current.live_in.Swap(live_in);
    for (const auto predecessor : block.predecessors) {
      if (queued.at(predecessor) == 0) {
        work.push_back(predecessor);
        queued.at(predecessor) = 1;
      }
    }
  }
  return live;
}

}  // namespace

std::vector<BlockLiveness> AnalyzeLiveness(const Function& function,
                                           const FlowGraph& graph,
                                           std::span<const Function> functions,
                                           std::span<const std::vector<RegisterId>> required,
                                           std::span<const RegisterId> uniform_required) {
  const auto transfers = SummarizeBlocks(function, graph, functions, required, uniform_required);
  return SolveLiveness<LivenessStorage::InputsAndOutputs>(
      function, graph, [&](std::size_t index, const auto& live, auto& live_in) {
        const auto& transfer = transfers.at(index);
        live_in.Subtract(transfer.killed);
        live_in.UnionWith(transfer.generated);
        if (transfer.exceptional) {
          const auto& exceptional = *transfer.exceptional;
          if (live.at(exceptional.successor).live_in.Has(exceptional.destination)) {
            live_in.Insert(exceptional.destination);
          }
        }
      });
}

std::vector<std::uint8_t> AnalyzeNecessaryLiveness(const Function& function,
                                                   const FlowGraph& graph,
                                                   std::span<const Function> functions) {
  struct Transfer final {
    RegisterAccess access;
    std::optional<std::size_t> exceptional;
    bool is_removable;
  };
  std::vector<Transfer> transfers;
  transfers.reserve(function.instructions.size());
  for (const auto& instruction : function.instructions) {
    const auto effects = AnalyzeEffects(instruction);
    transfers.push_back({
        .access = AnalyzeRegisters(instruction, functions),
        .exceptional = ExceptionalBlock(function, graph, instruction, effects),
        .is_removable = effects.IsRemovable(),
    });
  }
  std::vector<std::uint8_t> keep(function.instructions.size(), 0);
  static_cast<void>(SolveLiveness<LivenessStorage::Inputs>(
      function, graph, [&](std::size_t index, const auto& live, auto& live_in) {
        // 死指令是否产生 uses 依赖当前活性；随每轮传递覆盖标记，收敛后直接用于压缩。
        const auto& block = graph.blocks.at(index);
        for (auto position = block.end; position > block.begin;) {
          const auto& instruction = function.instructions.at(--position);
          const auto& transfer = transfers.at(position);
          const auto necessary = !transfer.access.defines_value || !transfer.is_removable ||
                                 live_in.Has(instruction.dst);
          keep.at(position) = necessary ? 1 : 0;
          if (necessary) {
            TransferInstruction(instruction, transfer.access, transfer.exceptional, live, live_in);
          }
        }
      }));
  return keep;
}

void TransferLiveness(const Function& function,
                      const FlowGraph& graph,
                      std::span<const BlockLiveness> blocks,
                      const Instruction& instruction,
                      const RegisterAccess& access,
                      RegisterSet& live,
                      LivenessRequirements requirements) {
  const auto exceptional = instruction.handler ? ExceptionalBlock(function, graph, instruction,
                                                                  AnalyzeEffects(instruction))
                                               : std::nullopt;
  TransferInstruction(instruction, access, exceptional, blocks, live);
  for (const auto reg : requirements.at_instruction) {
    live.Insert(reg);
  }
  for (const auto reg : requirements.uniform) {
    live.Insert(reg);
  }
}

void CompactInstructions(Function& function,
                         std::span<const std::uint8_t> keep,
                         std::vector<std::size_t>* origins) {
  if (std::ranges::find(keep, std::uint8_t{0}) == keep.end()) {
    return;
  }
  auto& code = function.instructions;
  std::vector<std::uint32_t> remap(code.size() + 1);
  std::uint32_t next = 0;
  for (std::size_t index = 0; index < code.size(); ++index) {
    remap.at(index) = next;
    next += keep[index] != 0 ? 1 : 0;
  }
  remap.back() = next;
  std::size_t destination = 0;
  for (std::size_t index = 0; index < code.size(); ++index) {
    if (keep[index] == 0) {
      continue;
    }
    auto& instruction = code.at(index);
    if (origins != nullptr) {
      origins->at(destination) = origins->at(index);
    }
    if (destination != index) {
      code.at(destination) = std::move(instruction);
    }
    ++destination;
  }
  code.resize(destination);
  if (origins != nullptr) {
    origins->resize(destination);
  }
  RemapJumpTargets(function, remap);
}

void RemapJumpTargets(Function& function, std::span<const std::uint32_t> targets) {
  for (auto& instruction : function.instructions) {
    if (IsJumpOpcode(instruction.opcode)) {
      instruction.index = targets[instruction.index];
    }
  }
  for (auto& handler : function.handlers) {
    handler.target = targets[handler.target];
  }
}

InstructionEffects AnalyzeEffects(const Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::LoadInt:
    case Opcode::LoadDouble:
    case Opcode::LoadString:
    case Opcode::Move:
    case Opcode::NotBool:
    case Opcode::NegDouble:
    case Opcode::EqualInt:
    case Opcode::NotEqualInt:
    case Opcode::EqualBool:
    case Opcode::NotEqualBool:
    case Opcode::EqualDouble:
    case Opcode::NotEqualDouble:
    case Opcode::EqualString:
    case Opcode::NotEqualString:
    case Opcode::EqualValue:
    case Opcode::NotEqualValue:
    case Opcode::LessInt:
    case Opcode::LessEqualInt:
    case Opcode::GreaterInt:
    case Opcode::GreaterEqualInt:
    case Opcode::LessDouble:
    case Opcode::LessEqualDouble:
    case Opcode::GreaterDouble:
    case Opcode::GreaterEqualDouble:
      return {};
    case Opcode::IsType:
    case Opcode::ExceptionValue:
      return {.reads_state = true};
    case Opcode::AddInt:
    case Opcode::SubInt:
    case Opcode::MulInt:
    case Opcode::DivInt:
    case Opcode::ModInt:
    case Opcode::NegInt:
    case Opcode::BitNotInt:
    case Opcode::BitAndInt:
    case Opcode::BitOrInt:
    case Opcode::BitXorInt:
    case Opcode::ShiftLeftInt:
    case Opcode::ShiftRightInt:
    case Opcode::ShiftUnsignedRightInt:
    case Opcode::AddDouble:
    case Opcode::SubDouble:
    case Opcode::MulDouble:
    case Opcode::DivDouble:
      return {.may_fail = true};
    case Opcode::CheckedCast:
    case Opcode::GetIndex:
      return {.may_fail = true, .reads_state = true};
    case Opcode::GetField:
      if (instruction.field_declaration) {
        return {
            .may_fail = true,
            .may_allocate = true,
            .reads_state = true,
            .writes_state = true,
        };
      }
      return {.may_fail = true, .reads_state = true};
    case Opcode::StringConcat:
    case Opcode::StringInterpolate:
    case Opcode::NewArray:
    case Opcode::NewDynamicArray:
    case Opcode::NewCollection:
    case Opcode::NewInstance:
    case Opcode::NewFuture:
    case Opcode::SleepFuture:
      return {.may_fail = true, .may_allocate = true, .reads_state = true, .writes_state = true};
    case Opcode::BuiltinCall:
      if (instruction.index == static_cast<std::uint32_t>(Builtin::NumericToInt) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::ScalarTextLength) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::NumericToDouble)) {
        // 数值转换不调用用户代码，只有类型或范围错误的失败封套可能分配。
        return {.may_fail = true};
      }
      if (instruction.index == static_cast<std::uint32_t>(Builtin::ArrayLength) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::MapGet) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::CollectionHas) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::CollectionSize) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::StringLength) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::StringByteLength) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::StringCodePointAt) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::ArrayCapacity) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::ArrayMutation) ||
          instruction.index == static_cast<std::uint32_t>(Builtin::ArrayCheckMutation)) {
        return {.may_fail = true, .reads_state = true};
      }
      [[fallthrough]];
    case Opcode::Call:
    case Opcode::CallIndirect:
    case Opcode::CallInterface:
    case Opcode::GetGlobal:
    case Opcode::AsyncCall:
      // 全局读取可能执行惰性初始化器，内建操作可能调用 getter/toJson/动态目标。
      // 没有叶调用证明时统一保留副作用，不能根据 opcode 的名字假设无 GC。
      return {
          .may_fail = true,
          .may_allocate = true,
          .reads_state = true,
          .writes_state = true,
      };
    case Opcode::Await:
      return {
          .may_fail = true,
          .may_allocate = true,
          .may_suspend = true,
          .reads_state = true,
          .writes_state = true,
          .changes_control = true,
      };
    case Opcode::SetField:
    case Opcode::SetIndex:
      return {.may_fail = true, .writes_state = true};
    case Opcode::SetGlobal:
    case Opcode::Print:
    case Opcode::Catch:
      return {.writes_state = true};
    case Opcode::Throw:
      return {
          .may_fail = true,
          .may_allocate = true,
          .writes_state = true,
          .changes_control = true,
      };
    case Opcode::Rethrow:
      return {
          .may_fail = true,
          .reads_state = true,
          .writes_state = true,
          .changes_control = true,
      };
    case Opcode::Jump:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
      return {.changes_control = true};
    case Opcode::Return:
      // 异步完成、擦除返回值装箱和载荷绑定可能失败。不能丢掉 return 的异常后继。
      return {
          .may_fail = true,
          .may_allocate = true,
          .reads_state = true,
          .writes_state = true,
          .changes_control = true,
      };
  }
  std::unreachable();
}

}  // namespace vela::ir
