#include "compiler/native/array_capacity_analysis.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "base/builtin.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {
namespace {

struct CapacityLoop final {
  std::size_t header;
  std::size_t latch;
  std::size_t step_begin;
  std::size_t preheader_block;
  std::size_t header_block;
  std::size_t latch_block;
  ir::RegisterId bound;
};

class CapacityAnalysis final {
 public:
  CapacityAnalysis(const ir::Function& function, std::span<const ir::RegisterAccess> accesses)
      : function_(&function),
        graph_(ir::BuildFlowGraph(function)),
        accesses_(accesses),
        register_flow_(ir::AnalyzeRegisterFlow(function, accesses_)) {}

  [[nodiscard]] std::vector<std::uint8_t> Run() const {
    std::vector<std::uint8_t> result(function_->instructions.size());
    for (std::size_t latch = 1; latch < result.size(); ++latch) {
      const auto loop = MatchLoop(latch);
      if (!loop) {
        continue;
      }
      const auto push = FindPush(*loop);
      if (push && HasReservedCapacity(*loop, *push)) {
        result[*push] = 1;
      }
    }
    return result;
  }

 private:
  [[nodiscard]] const ir::Instruction* Definition(ir::RegisterId reg) const {
    const auto position = register_flow_.OriginDefinition(reg);
    return position ? &function_->instructions[*position] : nullptr;
  }

  [[nodiscard]] bool HasSingleEntry(const CapacityLoop& loop) const {
    const auto& blocks = graph_.blocks;
    if (blocks[loop.header_block].begin != loop.header ||
        blocks[loop.preheader_block].end != loop.header ||
        std::ranges::any_of(blocks[loop.header_block].predecessors, [&](const auto predecessor) {
          return predecessor != loop.preheader_block && predecessor != loop.latch_block;
        })) {
      return false;
    }
    for (auto block = loop.header_block + 1; block <= loop.latch_block; ++block) {
      for (const auto predecessor : blocks[block].predecessors) {
        if (predecessor < loop.header_block || predecessor > loop.latch_block) {
          return false;
        }
      }
    }
    return true;
  }

  [[nodiscard]] std::optional<CapacityLoop> MatchLoop(std::size_t latch) const {
    const auto& code = function_->instructions;
    const auto& jump = code[latch];
    const auto header = jump.index;
    if (jump.opcode != Opcode::Jump || header == 0 || header + 1 >= latch ||
        code[header].opcode != Opcode::LessInt || code[header + 1].opcode != Opcode::JumpIfFalse ||
        code[header + 1].lhs != code[header].dst || code[header + 1].index <= latch) {
      return std::nullopt;
    }
    const auto counter = code[header].lhs;
    const auto bound_register = code[header].rhs;
    const auto bound = register_flow_.origins[bound_register];
    const auto& definitions = register_flow_.definitions[counter];
    const auto& step = code[latch - 1];
    if (!bound || definitions.count != 2 || !definitions.first || *definitions.first >= header ||
        code[*definitions.first].opcode != Opcode::LoadInt ||
        code[*definitions.first].immediate != 0 || step.opcode != Opcode::Move ||
        step.dst != counter || definitions.last != latch - 1) {
      return std::nullopt;
    }
    const auto& bound_definitions = register_flow_.definitions[*bound];
    const auto increment_register = step.lhs;
    const auto* increment = Definition(increment_register);
    if ((bound_definitions.first && *bound_definitions.first >= header) || increment == nullptr ||
        increment->opcode != Opcode::AddInt || increment->lhs != counter) {
      return std::nullopt;
    }
    const auto* increment_amount = Definition(increment->rhs);
    if (increment_amount == nullptr || increment_amount->opcode != Opcode::LoadInt ||
        increment_amount->immediate != 1) {
      return std::nullopt;
    }
    const auto increment_position = static_cast<std::size_t>(increment - code.data());
    if (increment_position != latch - 2) {
      return std::nullopt;
    }
    const auto increment_amount_position = static_cast<std::size_t>(increment_amount - code.data());
    const CapacityLoop loop{
        .header = header,
        .latch = latch,
        .step_begin = std::min(increment_position, increment_amount_position),
        .preheader_block = graph_.instruction_blocks[header - 1],
        .header_block = graph_.instruction_blocks[header],
        .latch_block = graph_.instruction_blocks[latch],
        .bound = *bound,
    };
    if (graph_.instruction_blocks[*definitions.first] != loop.preheader_block ||
        !HasSingleEntry(loop)) {
      return std::nullopt;
    }
    return loop;
  }

  [[nodiscard]] std::optional<std::size_t> FindPush(const CapacityLoop& loop) const {
    const auto& code = function_->instructions;
    std::optional<std::size_t> push;
    for (auto index = loop.header + 2; index < loop.latch; ++index) {
      const auto& instruction = code[index];
      if (ir::IsBuiltinCall(instruction, Builtin::ArrayPush)) {
        if (push) {
          return std::nullopt;
        }
        push = index;
        continue;
      }
      const auto effects = ir::AnalyzeEffects(instruction);
      if (effects.MayCollectOnSuccess() || effects.writes_state ||
          (effects.changes_control &&
           (!IsJumpOpcode(instruction.opcode) || instruction.index <= index ||
            (instruction.index > loop.step_begin && instruction.index <= loop.latch)))) {
        return std::nullopt;
      }
    }
    return push;
  }

  [[nodiscard]] bool HasReservedCapacity(const CapacityLoop& loop, std::size_t push) const {
    const auto& code = function_->instructions;
    const auto reg = code[push].lhs;
    const auto array = register_flow_.origins[reg];
    const auto* allocation = Definition(reg);
    if (!array || allocation == nullptr || allocation->opcode != Opcode::NewArray ||
        allocation->immediate != 0) {
      return false;
    }
    const auto allocation_position = static_cast<std::size_t>(allocation - code.data());
    if (graph_.instruction_blocks[allocation_position] != loop.preheader_block) {
      return false;
    }
    bool reserved = false;
    for (auto index = allocation_position + 1; index < loop.header; ++index) {
      const auto& instruction = code[index];
      const auto reserve = ir::IsBuiltinCall(instruction, Builtin::ArrayReserve) &&
                           register_flow_.origins[instruction.lhs] == array;
      reserved = reserved || (reserve && register_flow_.origins[instruction.lhs + 1] == loop.bound);
      const auto effects = ir::AnalyzeEffects(instruction);
      if (!reserve && (effects.MayCollectOnSuccess() || effects.writes_state)) {
        return false;
      }
      bool escaped = false;
      ir::ForEachUse(instruction, accesses_[index], [&](ir::RegisterId used) {
        escaped = escaped || (register_flow_.origins[used] == array &&
                              instruction.opcode != Opcode::Move && !reserve);
      });
      if (escaped) {
        return false;
      }
    }
    return reserved;
  }

  const ir::Function* function_;
  ir::FlowGraph graph_;
  std::span<const ir::RegisterAccess> accesses_;
  ir::RegisterFlow register_flow_;
};

}  // namespace

std::vector<std::uint8_t> FindReservedArrayPushes(const ir::Function& function,
                                                  std::span<const ir::RegisterAccess> accesses) {
  if (!ir::CanAnalyzeSynchronousFunction(function) ||
      std::ranges::none_of(function.instructions, [](const auto& instruction) {
        return ir::IsBuiltinCall(instruction, Builtin::ArrayPush);
      })) {
    return std::vector<std::uint8_t>(function.instructions.size());
  }
  return CapacityAnalysis(function, accesses).Run();
}

}  // namespace vela::compiler::native
