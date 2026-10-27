#include "compiler/ir/analysis.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

RegisterAccess AnalyzeRegisters(const Instruction& instruction,
                                std::span<const Function> functions) {
  using Field = Operand;
  switch (instruction.opcode) {
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::LoadInt:
    case Opcode::LoadIntConstant:
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
    case Opcode::NewOk:
    case Opcode::NewErr:
    case Opcode::ExpectedHasValue:
    case Opcode::ExpectedValue:
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
          .argument_count = functions.subspan(instruction.index).front().parameter_count,
      };
    case Opcode::CallIndirect:
      return {
          .defines_value = true,
          .uses = {Field::Right},
          .use_count = 1,
          .argument_count = instruction.index,
      };
    case Opcode::CallInterface:
      return {
          .defines_value = true,
          .argument_count = static_cast<std::uint32_t>(instruction.immediate),
      };
    case Opcode::BuiltinCall:
      return {
          .defines_value = true,
          .argument_count =
              static_cast<std::uint32_t>(BuiltinArity(static_cast<Builtin>(instruction.index))),
      };
    case Opcode::AsyncCall:
      return {
          .defines_value = true,
          .argument_count = static_cast<std::uint32_t>(
              AsyncArity(static_cast<AsyncOperation>(instruction.index))),
      };
    case Opcode::StringInterpolate:
      return {.defines_value = true, .argument_count = instruction.rhs};
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
    const auto& instruction = code.subspan(index).front();
    const auto opcode = instruction.opcode;
    const auto effects = AnalyzeEffects(instruction);
    if (opcode == Opcode::Jump || opcode == Opcode::JumpIfFalse || opcode == Opcode::JumpIfNull) {
      if (instruction.index < code.size()) {
        starts.at(instruction.index) = 1;
      }
    }
    if ((opcode == Opcode::Jump || opcode == Opcode::JumpIfFalse || opcode == Opcode::JumpIfNull ||
         opcode == Opcode::Return || opcode == Opcode::Throw || opcode == Opcode::Rethrow ||
         effects.may_suspend || (instruction.handler && effects.may_fail)) &&
        index + 1 < code.size()) {
      starts.at(index + 1) = 1;
    }
    if (index != 0 && instruction.handler != code.subspan(index - 1).front().handler) {
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
    if ((last.opcode == Opcode::Jump || last.opcode == Opcode::JumpIfFalse ||
         last.opcode == Opcode::JumpIfNull) &&
        last.index < code.size()) {
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

std::vector<BlockLiveness> AnalyzeLiveness(const Function& function,
                                           const FlowGraph& graph,
                                           std::span<const Function> functions,
                                           LivenessMode mode) {
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
          AnalyzeEffects(instruction).IsRemovable() && !live_in.contains(instruction.dst)) {
        continue;
      }
      TransferLiveness(function, graph, live, instruction, access, live_in);
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

void TransferLiveness(const Function& function,
                      const FlowGraph& graph,
                      std::span<const BlockLiveness> blocks,
                      const Instruction& instruction,
                      const RegisterAccess& access,
                      RegisterSet& live) {
  if (access.defines_value) {
    live.erase(instruction.dst);
  }
  if (instruction.handler && AnalyzeEffects(instruction).may_fail) {
    const auto target = function.handlers.at(*instruction.handler).target;
    const auto handler = graph.instruction_blocks.at(target);
    const auto& exceptional = blocks.subspan(handler).front().live_in;
    live.insert(exceptional.begin(), exceptional.end());
  }
  ForEachUse(instruction, access, [&](RegisterId reg) { live.insert(reg); });
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
    if (instruction.opcode == Opcode::Jump || instruction.opcode == Opcode::JumpIfFalse ||
        instruction.opcode == Opcode::JumpIfNull) {
      instruction.index = remap.at(instruction.index);
    }
    code.at(destination++) = instruction;
  }
  code.resize(destination);
  for (auto& handler : function.handlers) {
    handler.target = remap.at(handler.target);
  }
}

InstructionEffects AnalyzeEffects(const Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::LoadInt:
    case Opcode::LoadIntConstant:
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
    case Opcode::GetField:
    case Opcode::ExpectedHasValue:
    case Opcode::ExpectedValue:
      return {.may_fail = true, .reads_state = true};
    case Opcode::StringConcat:
    case Opcode::StringInterpolate:
    case Opcode::NewArray:
    case Opcode::NewDynamicArray:
    case Opcode::NewCollection:
    case Opcode::NewInstance:
    case Opcode::NewOk:
    case Opcode::NewErr:
    case Opcode::NewFuture:
    case Opcode::SleepFuture:
      return {.may_fail = true, .may_allocate = true, .reads_state = true, .writes_state = true};
    case Opcode::Call:
    case Opcode::CallIndirect:
    case Opcode::CallInterface:
    case Opcode::GetGlobal:
    case Opcode::BuiltinCall:
    case Opcode::AsyncCall:
      // 全局读取可能执行惰性初始化器，内建操作可能调用 getter/toJson/动态目标。
      // 没有叶调用证明时统一保留副作用，不能根据 opcode 的名字假设无 GC。
      return {.may_fail = true,
              .may_allocate = true,
              .may_call = true,
              .reads_state = true,
              .writes_state = true};
    case Opcode::Await:
      return {.may_fail = true,
              .may_allocate = true,
              .may_suspend = true,
              .reads_state = true,
              .writes_state = true,
              .changes_control = true};
    case Opcode::SetField:
    case Opcode::SetIndex:
      return {.may_fail = true, .writes_state = true};
    case Opcode::SetGlobal:
    case Opcode::Print:
    case Opcode::Catch:
      return {.writes_state = true};
    case Opcode::Throw:
      return {
          .may_fail = true, .may_allocate = true, .writes_state = true, .changes_control = true};
    case Opcode::Rethrow:
      return {.may_fail = true, .reads_state = true, .writes_state = true, .changes_control = true};
    case Opcode::Jump:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
      return {.changes_control = true};
    case Opcode::Return:
      // 异步完成、擦除返回值装箱和载荷绑定可能失败。不能丢掉 return 的异常后继。
      return {.may_fail = true,
              .may_allocate = true,
              .reads_state = true,
              .writes_state = true,
              .changes_control = true};
  }
  std::unreachable();
}

}  // namespace vela::ir
