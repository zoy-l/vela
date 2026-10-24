#include "compiler/ir/optimizer.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

using Constant = std::variant<std::monostate, bool, std::int64_t>;
using Constants = absl::flat_hash_map<RegisterId, Constant>;

auto EvaluateIntegers(Opcode opcode, std::int64_t left, std::int64_t right)
    -> std::optional<Constant> {
  std::int64_t value = 0;
  switch (opcode) {
    case Opcode::AddInt:
      return __builtin_add_overflow(left, right, &value) ? std::nullopt
                                                         : std::optional<Constant>(value);
    case Opcode::SubInt:
      return __builtin_sub_overflow(left, right, &value) ? std::nullopt
                                                         : std::optional<Constant>(value);
    case Opcode::MulInt:
      return __builtin_mul_overflow(left, right, &value) ? std::nullopt
                                                         : std::optional<Constant>(value);
    case Opcode::DivInt:
      if (right == 0 || (left == std::numeric_limits<std::int64_t>::min() && right == -1)) {
        return std::nullopt;
      }
      return left / right;
    case Opcode::EqualInt:
      return left == right;
    case Opcode::NotEqualInt:
      return left != right;
    case Opcode::LessInt:
      return left < right;
    case Opcode::LessEqualInt:
      return left <= right;
    case Opcode::GreaterInt:
      return left > right;
    case Opcode::GreaterEqualInt:
      return left >= right;
    default:
      return std::nullopt;
  }
}

auto EvaluateUnary(const Instruction& instruction, const Constant& operand)
    -> std::optional<Constant> {
  if (instruction.opcode == Opcode::Move) {
    return operand;
  }
  if (instruction.opcode == Opcode::NotBool && std::holds_alternative<bool>(operand)) {
    return !std::get<bool>(operand);
  }
  if (instruction.opcode == Opcode::NegInt && std::holds_alternative<std::int64_t>(operand)) {
    const auto value = std::get<std::int64_t>(operand);
    if (value != std::numeric_limits<std::int64_t>::min()) {
      return -value;
    }
  }
  return std::nullopt;
}

auto EvaluateBinary(const Instruction& instruction, const Constant& left, const Constant& right)
    -> std::optional<Constant> {
  const auto opcode = instruction.opcode;
  if (opcode == Opcode::EqualValue) {
    return left == right;
  }
  if (opcode == Opcode::NotEqualValue) {
    return left != right;
  }
  if (std::holds_alternative<std::int64_t>(left) && std::holds_alternative<std::int64_t>(right)) {
    return EvaluateIntegers(opcode, std::get<std::int64_t>(left), std::get<std::int64_t>(right));
  }
  if (!std::holds_alternative<bool>(left) || !std::holds_alternative<bool>(right)) {
    return std::nullopt;
  }
  const auto lhs = std::get<bool>(left);
  const auto rhs = std::get<bool>(right);
  switch (opcode) {
    case Opcode::EqualBool:
      return lhs == rhs;
    case Opcode::NotEqualBool:
      return lhs != rhs;
    case Opcode::AndBool:
      return lhs && rhs;
    case Opcode::OrBool:
      return lhs || rhs;
    default:
      return std::nullopt;
  }
}

auto Evaluate(const Instruction& instruction, const Constants& constants)
    -> std::optional<Constant> {
  switch (instruction.opcode) {
    case Opcode::LoadNull:
      return Constant();
    case Opcode::LoadBool:
      return instruction.immediate != 0;
    case Opcode::LoadInt:
      return instruction.immediate;
    default:
      break;
  }
  const auto left = constants.find(instruction.lhs);
  if (left == constants.end()) {
    return std::nullopt;
  }
  if (instruction.opcode == Opcode::Move || instruction.opcode == Opcode::NegInt ||
      instruction.opcode == Opcode::NotBool) {
    return EvaluateUnary(instruction, left->second);
  }
  const auto right = constants.find(instruction.rhs);
  return right == constants.end() ? std::nullopt
                                  : EvaluateBinary(instruction, left->second, right->second);
}

void Materialize(Instruction& instruction, const Constant& value) {
  if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    // 当前整数常量编码为 int16；较大运算结果保留原运算，不能把有效程序折叠成编码错误。
    if (*integer < std::numeric_limits<std::int16_t>::min() ||
        *integer > std::numeric_limits<std::int16_t>::max()) {
      return;
    }
    instruction.opcode = Opcode::LoadInt;
    instruction.immediate = *integer;
  } else if (const auto* boolean = std::get_if<bool>(&value)) {
    instruction.opcode = Opcode::LoadBool;
    instruction.immediate = *boolean ? 1 : 0;
  } else {
    instruction.opcode = Opcode::LoadNull;
    instruction.immediate = 0;
  }
  instruction.lhs = 0;
  instruction.rhs = 0;
  instruction.index = 0;
}

void FoldConstants(Function& function, std::span<const Function> functions) {
  const auto graph = BuildFlowGraph(function);
  std::vector<std::uint8_t> keep(function.instructions.size(), 1);
  for (const auto& block : graph.blocks) {
    Constants constants;
    for (std::size_t index = block.begin; index < block.end; ++index) {
      auto& instruction = function.instructions.at(index);
      if (instruction.opcode == Opcode::JumpIfFalse) {
        const auto condition = constants.find(instruction.lhs);
        if (condition != constants.end() && std::holds_alternative<bool>(condition->second)) {
          if (std::get<bool>(condition->second)) {
            keep.at(index) = 0;
          } else {
            instruction.opcode = Opcode::Jump;
            instruction.lhs = 0;
          }
        }
        continue;
      }
      const auto access = AnalyzeRegisters(instruction, functions);
      if (!access.defines_value) {
        continue;
      }
      const auto value = Evaluate(instruction, constants);
      if (value) {
        constants.insert_or_assign(instruction.dst, *value);
        Materialize(instruction, *value);
      } else {
        constants.erase(instruction.dst);
      }
    }
  }
  CompactInstructions(function, keep);
}

void RemoveUnreachable(Function& function) {
  const auto graph = BuildFlowGraph(function);
  if (graph.blocks.empty()) {
    return;
  }
  std::vector<std::uint8_t> reached(graph.blocks.size(), 0);
  std::vector<std::size_t> work = {0};
  reached.front() = 1;
  while (!work.empty()) {
    const auto index = work.back();
    work.pop_back();
    for (const auto successor : graph.blocks.at(index).successors) {
      if (reached.at(successor) == 0) {
        reached.at(successor) = 1;
        work.push_back(successor);
      }
    }
  }
  std::vector<std::uint8_t> keep(function.instructions.size(), 0);
  for (std::size_t index = 0; index < keep.size(); ++index) {
    keep.at(index) = reached.at(graph.instruction_blocks.at(index));
  }
  CompactInstructions(function, keep);
}

void RemoveUnusedValues(Function& function, std::span<const Function> functions) {
  const auto graph = BuildFlowGraph(function);
  const auto blocks =
      AnalyzeLiveness(function, graph, functions, LivenessMode::NecessaryInstructions);
  std::vector<std::uint8_t> keep(function.instructions.size(), 1);
  for (std::size_t block_index = 0; block_index < graph.blocks.size(); ++block_index) {
    auto live = blocks.at(block_index).live_out;
    const auto& block = graph.blocks.at(block_index);
    for (std::size_t index = block.end; index > block.begin;) {
      --index;
      const auto& instruction = function.instructions.at(index);
      const auto access = AnalyzeRegisters(instruction, functions);
      if (access.defines_value && IsRemovableInstruction(instruction.opcode) &&
          !live.contains(instruction.dst)) {
        keep.at(index) = 0;
        continue;
      }
      if (access.defines_value) {
        live.erase(instruction.dst);
      }
      ForEachUse(instruction, access, [&](RegisterId reg) -> void { live.insert(reg); });
    }
  }
  CompactInstructions(function, keep);
}

}  // namespace

void Optimize(Function& function, std::span<const Function> functions) {
  FoldConstants(function, functions);
  RemoveUnreachable(function);
  RemoveUnusedValues(function, functions);
}

}  // namespace vela::ir
