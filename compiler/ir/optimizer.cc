#include "compiler/ir/optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/builtin.h"
#include "base/integer_arithmetic.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

using Constant = std::variant<std::monostate, bool, std::int64_t>;
using Constants = absl::flat_hash_map<RegisterId, Constant>;

void InvalidateRepresentative(RegisterId destination,
                              const absl::flat_hash_map<RegisterId, std::size_t>& values,
                              absl::flat_hash_map<std::size_t, RegisterId>& representatives) {
  const auto previous = values.find(destination);
  if (previous == values.end()) {
    return;
  }
  const auto representative = representatives.find(previous->second);
  if (representative != representatives.end() && representative->second == destination) {
    representatives.erase(representative);
  }
}

void PropagateCopies(Function& function,
                     std::span<const Function> functions,
                     const FlowGraph& graph,
                     std::span<std::uint8_t> keep) {
  absl::flat_hash_map<RegisterId, std::size_t> values;
  absl::flat_hash_map<std::size_t, RegisterId> representatives;
  for (const auto& block : graph.blocks) {
    // Abseil clear() 可释放较大表的容量；整段 erase 保留桶数组供后续块复用。
    values.erase(values.begin(), values.end());
    representatives.erase(representatives.begin(), representatives.end());
    std::size_t next_value = 0;
    const auto value_of = [&](RegisterId reg) -> std::size_t {
      const auto [entry, inserted] = values.try_emplace(reg, next_value);
      if (inserted) {
        ++next_value;
      }
      return entry->second;
    };
    for (auto index = block.begin; index < block.end; ++index) {
      auto& instruction = function.instructions.at(index);
      const auto access = AnalyzeRegisters(instruction, functions);
      for (const auto operand : std::span(access.uses).first(access.use_count)) {
        const auto reg = ReadOperand(instruction, operand);
        const auto value = value_of(reg);
        const auto [representative, inserted] = representatives.try_emplace(value, reg);
        static_cast<void>(inserted);
        WriteOperand(instruction, operand, representative->second);
      }
      if (!access.defines_value) {
        continue;
      }
      const auto destination = instruction.dst;
      const auto source_value = instruction.opcode == Opcode::Move
                                    ? std::optional(value_of(instruction.lhs))
                                    : std::nullopt;
      const auto previous = values.find(destination);
      if (source_value && previous != values.end() && previous->second == *source_value) {
        keep.subspan(index).front() = 0;
        continue;
      }
      InvalidateRepresentative(destination, values, representatives);
      const auto value = source_value ? *source_value : next_value++;
      values.insert_or_assign(destination, value);
      representatives.try_emplace(value, destination);
    }
  }
}

void ThreadJumps(Function& function, std::vector<std::size_t>* origins) {
  const auto count = function.instructions.size();
  std::vector<std::uint8_t> states(count, 0);
  std::vector<std::uint32_t> resolved(count);
  std::vector<std::uint32_t> path;
  for (std::size_t index = 0; index < count; ++index) {
    if (function.instructions.at(index).opcode != Opcode::Jump || states.at(index) != 0) {
      continue;
    }
    auto target = static_cast<std::uint32_t>(index);
    path.clear();
    while (target < count && function.instructions.at(target).opcode == Opcode::Jump &&
           states.at(target) == 0) {
      states.at(target) = 1;
      path.push_back(target);
      target = function.instructions.at(target).index;
    }
    if (target < count && states.at(target) == 2) {
      target = resolved.at(target);
    }
    for (const auto jump : path) {
      states.at(jump) = 2;
      resolved.at(jump) = target;
    }
  }
  std::vector<std::uint8_t> keep(count, 1);
  for (std::size_t index = 0; index < count; ++index) {
    auto& instruction = function.instructions.at(index);
    if (instruction.opcode != Opcode::Jump && instruction.opcode != Opcode::JumpIfFalse &&
        instruction.opcode != Opcode::JumpIfNull) {
      continue;
    }
    const auto target = instruction.index;
    if (target < count && states.at(target) == 2) {
      instruction.index = resolved.at(target);
    }
    if (instruction.opcode == Opcode::Jump && instruction.index == index + 1) {
      keep.at(index) = 0;
    }
  }
  CompactInstructions(function, keep, origins);
}

std::optional<Constant> EvaluateIntegers(Opcode opcode, std::int64_t left, std::int64_t right) {
  switch (opcode) {
    case Opcode::AddInt:
    case Opcode::SubInt:
    case Opcode::MulInt:
    case Opcode::DivInt: {
      const auto value = EvaluateIntegerArithmetic(opcode, left, right);
      return value ? std::optional<Constant>(*value) : std::nullopt;
    }
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

std::optional<Constant> EvaluateUnary(const Instruction& instruction, const Constant& operand) {
  if (instruction.opcode == Opcode::Move) {
    return operand;
  }
  if (instruction.opcode == Opcode::NotBool && std::holds_alternative<bool>(operand)) {
    return !std::get<bool>(operand);
  }
  if (instruction.opcode == Opcode::NegInt && std::holds_alternative<std::int64_t>(operand)) {
    const auto value =
        EvaluateIntegerArithmetic(Opcode::SubInt, 0, std::get<std::int64_t>(operand));
    return value ? std::optional<Constant>(*value) : std::nullopt;
  }
  return std::nullopt;
}

std::optional<Constant> EvaluateBinary(const Instruction& instruction,
                                       const Constant& left,
                                       const Constant& right) {
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

std::optional<Constant> Evaluate(const Instruction& instruction, const Constants& constants) {
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

void FoldBranch(Instruction& instruction, const Constants& constants, std::uint8_t& keep) {
  const auto condition = constants.find(instruction.lhs);
  if (condition == constants.end() || !std::holds_alternative<bool>(condition->second)) {
    return;
  }
  if (std::get<bool>(condition->second)) {
    keep = 0;
  } else {
    instruction.opcode = Opcode::Jump;
    instruction.lhs = 0;
  }
}

void FoldConstants(Function& function,
                   std::span<const Function> functions,
                   const FlowGraph& graph,
                   std::span<std::uint8_t> keep) {
  Constants constants;
  for (const auto& block : graph.blocks) {
    constants.erase(constants.begin(), constants.end());
    for (std::size_t index = block.begin; index < block.end; ++index) {
      auto& retained = keep.subspan(index).front();
      if (retained == 0) {
        continue;
      }
      auto& instruction = function.instructions.at(index);
      if (instruction.opcode == Opcode::JumpIfFalse) {
        FoldBranch(instruction, constants, retained);
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
}

FlowGraph RemoveUnreachable(Function& function, std::vector<std::size_t>* origins) {
  auto graph = BuildFlowGraph(function);
  if (graph.blocks.empty()) {
    return graph;
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
  if (std::ranges::all_of(reached, [](auto value) { return value != 0; })) {
    return graph;
  }
  std::vector<std::uint8_t> keep(function.instructions.size(), 0);
  for (std::size_t index = 0; index < keep.size(); ++index) {
    keep.at(index) = reached.at(graph.instruction_blocks.at(index));
  }
  CompactInstructions(function, keep, origins);
  return BuildFlowGraph(function);
}

void RemoveUnusedValues(Function& function,
                        std::span<const Function> functions,
                        const FlowGraph& graph,
                        std::vector<std::size_t>* origins) {
  const auto liveness = AnalyzeNecessaryLiveness(function, graph, functions);
  CompactInstructions(function, liveness.keep, origins);
}

void RemoveUnusedHandlers(Function& function) {
  std::vector<std::optional<std::uint32_t>> remap(function.handlers.size());
  std::vector<Function::Handler> handlers;
  for (auto& instruction : function.instructions) {
    const auto previous = instruction.handler;
    if (!previous) {
      continue;
    }
    if (!AnalyzeEffects(instruction).may_fail) {
      instruction.handler.reset();
      continue;
    }
    auto& target = remap.at(*previous);
    if (!target) {
      target = static_cast<std::uint32_t>(handlers.size());
      handlers.push_back(function.handlers.at(*previous));
    }
    instruction.handler = *target;
  }
  function.handlers = std::move(handlers);
}

bool CanMeasureScalarText(const frontend::TypeDescriptor& input) {
  using Kind = frontend::TypeDescriptor::Kind;
  return !input.nullable &&
         (input.kind == Kind::Int || input.kind == Kind::Double || input.kind == Kind::Bool);
}

void FuseScalarTextLengths(Function& function, std::span<const Function> functions) {
  std::vector<std::size_t> definitions(function.register_count);
  std::vector<std::size_t> uses(function.register_count);
  std::vector<std::size_t> consumers(function.register_count);
  for (std::size_t reg = 0; reg < function.parameter_count; ++reg) {
    definitions.at(reg) = 1;
  }
  auto& code = function.instructions;
  for (std::size_t index = 0; index < code.size(); ++index) {
    const auto& instruction = code.at(index);
    const auto access = AnalyzeRegisters(instruction, functions);
    if (access.defines_value) {
      ++definitions.at(instruction.dst);
    }
    ForEachUse(instruction, access, [&](RegisterId reg) {
      ++uses.at(reg);
      consumers.at(reg) = index;
    });
  }
  using Kind = frontend::TypeDescriptor::Kind;
  for (auto& producer : code) {
    if (producer.opcode != Opcode::BuiltinCall ||
        producer.index != static_cast<std::uint32_t>(Builtin::ScalarToString)) {
      continue;
    }
    const auto& input = function.value_types.at(producer.lhs);
    if (!CanMeasureScalarText(input)) {
      continue;
    }
    std::vector<RegisterId> path;
    auto reg = producer.dst;
    while (definitions.at(reg) == 1 && uses.at(reg) == 1) {
      path.push_back(reg);
      auto& consumer = code.at(consumers.at(reg));
      if (consumer.opcode == Opcode::Move) {
        reg = consumer.dst;
        continue;
      }
      if (consumer.opcode == Opcode::BuiltinCall &&
          (consumer.index == static_cast<std::uint32_t>(Builtin::StringLength) ||
           consumer.index == static_cast<std::uint32_t>(Builtin::StringByteLength))) {
        // 标量文本只含 ASCII；唯一消费是长度时，不产生可逃逸的字符串身份。
        // 在格式化的原位置计算长度，并保留 Move 链，避免跨越输入改写或控制流。
        producer.index = static_cast<std::uint32_t>(Builtin::ScalarTextLength);
        consumer.opcode = Opcode::Move;
        consumer.index = 0;
        for (const auto value : path) {
          function.value_types.at(value) = {.kind = Kind::Int, .name = frontend::kIntTypeName};
        }
      }
      break;
    }
  }
}

}  // namespace

void Optimize(Function& function,
              std::span<const Function> functions,
              std::vector<std::size_t>* origins) {
  {
    // 局部传播只依赖原始基本块边界；两个 pass 完成后统一压缩，保持指令编号稳定。
    const auto graph = BuildFlowGraph(function);
    std::vector<std::uint8_t> keep(function.instructions.size(), 1);
    PropagateCopies(function, functions, graph, keep);
    FoldConstants(function, functions, graph, keep);
    CompactInstructions(function, keep, origins);
  }
  ThreadJumps(function, origins);
  const auto graph = RemoveUnreachable(function, origins);
  RemoveUnusedValues(function, functions, graph, origins);
  FuseScalarTextLengths(function, functions);
  RemoveUnusedHandlers(function);
}

}  // namespace vela::ir
