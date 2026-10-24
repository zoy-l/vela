#include "compiler/ir/analysis.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

auto AnalyzeRegisters(const Instruction& instruction, std::span<const Function> functions)
    -> RegisterAccess {
  using Field = Operand;
  switch (instruction.opcode) {
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::LoadInt:
    case Opcode::LoadDouble:
    case Opcode::LoadString:
    case Opcode::NewArray:
    case Opcode::NewInstance:
      return {.defines_value = true};
    case Opcode::Move:
    case Opcode::NegInt:
    case Opcode::NegDouble:
    case Opcode::NotBool:
    case Opcode::GetField:
      return {.defines_value = true, .uses = {Field::Left}, .use_count = 1};
    case Opcode::SetField:
      return {.uses = {Field::Destination, Field::Left}, .use_count = 2};
    case Opcode::SetIndex:
      return {.uses = {Field::Destination, Field::Left, Field::Right}, .use_count = 3};
    case Opcode::Print:
    case Opcode::JumpIfFalse:
      return {.uses = {Field::Left}, .use_count = 1};
    case Opcode::Return:
      return {.uses = {Field::Destination}, .use_count = 1};
    case Opcode::Jump:
      return {};
    case Opcode::Call:
      return {
          .defines_value = true,
          .argument_count = functions.subspan(instruction.index).front().parameter_count,
      };
    case Opcode::BuiltinCall:
      return {
          .defines_value = true,
          .argument_count =
              static_cast<std::uint32_t>(BuiltinArity(static_cast<Builtin>(instruction.index))),
      };
    case Opcode::StringInterpolate:
      return {.defines_value = true, .argument_count = instruction.rhs};
    case Opcode::AddInt:
    case Opcode::SubInt:
    case Opcode::MulInt:
    case Opcode::DivInt:
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
    case Opcode::AndBool:
    case Opcode::OrBool:
    case Opcode::StringConcat:
    case Opcode::EqualString:
    case Opcode::NotEqualString:
    case Opcode::GetIndex:
      return {.defines_value = true, .uses = {Field::Left, Field::Right}, .use_count = 2};
  }
  std::unreachable();
}

auto ReadOperand(const Instruction& instruction, Operand operand) -> RegisterId {
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

auto FindBlockStarts(std::span<const Instruction> code) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> starts(code.size(), 0);
  starts.front() = 1;
  for (std::size_t index = 0; index < code.size(); ++index) {
    const auto& instruction = code.subspan(index).front();
    const auto opcode = instruction.opcode;
    if (opcode == Opcode::Jump || opcode == Opcode::JumpIfFalse) {
      if (instruction.index < code.size()) {
        starts.at(instruction.index) = 1;
      }
    }
    if ((opcode == Opcode::Jump || opcode == Opcode::JumpIfFalse || opcode == Opcode::Return) &&
        index + 1 < code.size()) {
      starts.at(index + 1) = 1;
    }
  }
  return starts;
}

}  // namespace

auto BuildFlowGraph(const Function& function) -> FlowGraph {
  FlowGraph graph;
  const auto& code = function.instructions;
  if (code.empty()) {
    return graph;
  }
  const auto starts = FindBlockStarts(code);
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
    if ((last.opcode == Opcode::Jump || last.opcode == Opcode::JumpIfFalse) &&
        last.index < code.size()) {
      block.successors.push_back(graph.instruction_blocks.at(last.index));
    }
    if (last.opcode != Opcode::Jump && last.opcode != Opcode::Return &&
        index + 1 < graph.blocks.size()) {
      block.successors.push_back(index + 1);
    }
    for (const auto successor : block.successors) {
      graph.blocks.at(successor).predecessors.push_back(index);
    }
  }
  return graph;
}

auto AnalyzeLiveness(const Function& function,
                     const FlowGraph& graph,
                     std::span<const Function> functions,
                     LivenessMode mode) -> std::vector<BlockLiveness> {
  std::vector<BlockLiveness> live(graph.blocks.size());
  std::vector<std::size_t> work;
  work.reserve(graph.blocks.size());
  std::vector<std::uint8_t> queued(graph.blocks.size(), 1);
  for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
    work.push_back(index);
  }
  while (!work.empty()) {
    const auto index = work.back();
    work.pop_back();
    queued.at(index) = 0;
    auto& current = live.at(index);
    RegisterSet live_out;
    const auto& block = graph.blocks.at(index);
    for (const auto successor : block.successors) {
      const auto& next = live.at(successor).live_in;
      live_out.insert(next.begin(), next.end());
    }
    auto live_in = live_out;
    for (std::size_t position = block.end; position > block.begin;) {
      const auto& instruction = function.instructions.at(--position);
      const auto access = AnalyzeRegisters(instruction, functions);
      if (mode == LivenessMode::NecessaryInstructions && access.defines_value &&
          IsRemovableInstruction(instruction.opcode) && !live_in.contains(instruction.dst)) {
        continue;
      }
      if (access.defines_value) {
        live_in.erase(instruction.dst);
      }
      ForEachUse(instruction, access, [&](RegisterId reg) -> void { live_in.insert(reg); });
    }
    current.live_out = std::move(live_out);
    if (current.live_in == live_in) {
      continue;
    }
    current.live_in = std::move(live_in);
    for (const auto predecessor : block.predecessors) {
      if (queued.at(predecessor) == 0) {
        work.push_back(predecessor);
        queued.at(predecessor) = 1;
      }
    }
  }
  return live;
}

void CompactInstructions(Function& function, std::span<const std::uint8_t> keep) {
  auto& code = function.instructions;
  std::vector<std::uint32_t> remap(code.size() + 1);
  std::uint32_t next = 0;
  for (std::size_t index = 0; index < code.size(); ++index) {
    remap.at(index) = next;
    next += keep.subspan(index).front() != 0 ? 1 : 0;
  }
  remap.back() = next;
  std::size_t destination = 0;
  for (std::size_t index = 0; index < code.size(); ++index) {
    if (keep.subspan(index).front() == 0) {
      continue;
    }
    auto instruction = code.at(index);
    if (instruction.opcode == Opcode::Jump || instruction.opcode == Opcode::JumpIfFalse) {
      instruction.index = remap.at(instruction.index);
    }
    code.at(destination++) = instruction;
  }
  code.resize(destination);
}

auto IsRemovableInstruction(Opcode opcode) -> bool {
  switch (opcode) {
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
    case Opcode::AndBool:
    case Opcode::OrBool:
      return true;
    default:
      return false;
  }
}

}  // namespace vela::ir
