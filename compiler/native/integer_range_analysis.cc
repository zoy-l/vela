#include "compiler/native/integer_range_analysis.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Operator.h>

#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_values.h"

namespace vela::compiler::native {

std::span<const std::uint64_t> IntegerRangeAnalysis::ArrayBits(std::size_t index) const {
  return array_bits_.empty() ? std::span<const std::uint64_t>() : array_bits_[index];
}

namespace {

using Range = llvm::ConstantRange;
using State = std::vector<Range>;
constexpr unsigned kIntegerBits = std::numeric_limits<std::uint64_t>::digits;
constexpr unsigned kProductBits = kIntegerBits * 2;

enum class RangeChange : std::uint8_t { Unchanged, Expanded };

Range Full() {
  return Range::getFull(kIntegerBits);
}
Range Empty() {
  return Range::getEmpty(kIntegerBits);
}
Range Constant(std::int64_t value) {
  return {llvm::APInt(kIntegerBits, std::bit_cast<std::uint64_t>(value))};
}

Range Interval(std::int64_t lower, std::int64_t upper) {
  return Range::getNonEmpty(llvm::APInt(kIntegerBits, std::bit_cast<std::uint64_t>(lower)),
                            llvm::APInt(kIntegerBits, std::bit_cast<std::uint64_t>(upper)) + 1);
}

bool IsInteger(const ir::Function& function, ir::RegisterId reg) {
  const auto& type = function.value_types[reg];
  return type.kind == frontend::TypeDescriptor::Kind::Int && !type.nullable;
}

std::vector<std::int64_t> WideningThresholds(const ir::Program& program) {
  std::vector<std::int64_t> thresholds{
      std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max(),
      0,
  };
  for (const auto& function : program.functions) {
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode != Opcode::LoadInt) {
        continue;
      }
      const auto value = instruction.immediate;
      thresholds.push_back(value);
      if (value != std::numeric_limits<std::int64_t>::min()) {
        thresholds.push_back(value - 1);
      }
      if (value != std::numeric_limits<std::int64_t>::max()) {
        thresholds.push_back(value + 1);
      }
    }
  }
  std::ranges::sort(thresholds);
  const auto duplicates = std::ranges::unique(thresholds);
  thresholds.erase(duplicates.begin(), duplicates.end());
  return thresholds;
}

RangeChange MergeRange(Range& destination,
                       const Range& added,
                       std::span<const std::int64_t> thresholds) {
  if (destination.contains(added)) {
    return RangeChange::Unchanged;
  }
  if (destination.isEmptySet()) {
    destination = added;
    return RangeChange::Expanded;
  }
  const auto lower =
      std::min(destination.getSignedMin().getSExtValue(), added.getSignedMin().getSExtValue());
  const auto upper =
      std::max(destination.getSignedMax().getSExtValue(), added.getSignedMax().getSExtValue());
  // 只向外扩张到源码常量阈值；递归和循环都在有限格上收敛，不截断迭代后猜测范围。
  const auto low = std::ranges::upper_bound(thresholds, lower);
  const auto high = std::ranges::lower_bound(thresholds, upper);
  destination = Interval(*std::prev(low), *high);
  return RangeChange::Expanded;
}

Range ResultRange(const ir::Instruction& instruction,
                  const State& state,
                  std::span<const std::uint64_t> arrays) {
  const auto opcode = instruction.opcode;
  const auto& left = state[instruction.lhs];
  const auto& right = state[instruction.rhs];
  switch (opcode) {
    case Opcode::LoadInt:
      return Constant(instruction.immediate);
    case Opcode::Move:
      return left;
    case Opcode::GetIndex:
      if (!arrays.empty()) {
        const auto array_bits = arrays[instruction.lhs];
        if (array_bits < std::bit_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return Interval(0, static_cast<std::int64_t>(array_bits));
        }
      }
      return Full();
    case Opcode::AddInt:
      return left.overflowingBinaryOp(llvm::Instruction::Add, right,
                                      llvm::OverflowingBinaryOperator::NoSignedWrap);
    case Opcode::SubInt:
      return left.overflowingBinaryOp(llvm::Instruction::Sub, right,
                                      llvm::OverflowingBinaryOperator::NoSignedWrap);
    case Opcode::MulInt:
      return left.overflowingBinaryOp(llvm::Instruction::Mul, right,
                                      llvm::OverflowingBinaryOperator::NoSignedWrap);
    case Opcode::NegInt:
      return Constant(0).overflowingBinaryOp(llvm::Instruction::Sub, left,
                                             llvm::OverflowingBinaryOperator::NoSignedWrap);
    case Opcode::BitAndInt:
      return left.binaryAnd(right);
    case Opcode::BitOrInt:
      return left.binaryOr(right);
    case Opcode::BitXorInt:
      return left.binaryXor(right);
    case Opcode::BitNotInt:
      return left.binaryXor(Constant(-1));
    default:
      return Full();
  }
}

bool HasNoOverflow(const ir::Instruction& instruction, const State& state) {
  const auto opcode = instruction.opcode;
  if (opcode != Opcode::AddInt && opcode != Opcode::SubInt && opcode != Opcode::MulInt &&
      opcode != Opcode::NegInt) {
    return false;
  }
  const auto& left = state[instruction.lhs];
  if (left.isEmptySet()) {
    return false;
  }
  const auto never = Range::OverflowResult::NeverOverflows;
  if (opcode == Opcode::NegInt) {
    return Constant(0).signedSubMayOverflow(left) == never;
  }
  const auto& right = state[instruction.rhs];
  if (right.isEmptySet()) {
    return false;
  }
  switch (opcode) {
    case Opcode::AddInt:
      return left.signedAddMayOverflow(right) == never;
    case Opcode::SubInt:
      return left.signedSubMayOverflow(right) == never;
    case Opcode::MulInt: {
      const auto product = left.signExtend(kProductBits).multiply(right.signExtend(kProductBits));
      return product.getSignedMin().isSignedIntN(kIntegerBits) &&
             product.getSignedMax().isSignedIntN(kIntegerBits);
    }
    default:
      return false;
  }
}

}  // namespace

IntegerRangeAnalysis::IntegerRangeAnalysis(
    const ir::Program& program,
    std::span<const std::uint8_t> roots,
    std::span<const std::vector<ir::RegisterAccess>> accesses)
    : program_(&program),
      roots_(roots),
      accesses_(accesses),
      thresholds_(WideningThresholds(program)),
      facts_(program.functions.size()),
      flows_(program.functions.size()),
      queued_(program.functions.size()) {}

std::span<IntegerRangeFacts> IntegerRangeAnalysis::Run(
    std::span<const std::vector<std::uint64_t>> array_bits) {
  array_bits_ = array_bits;
  for (std::size_t index = 0; index < facts_.size(); ++index) {
    auto& facts = facts_[index];
    facts.reachable = roots_[index] != 0;
    facts.parameters.assign(program_->functions[index].parameter_count,
                            facts.reachable ? Full() : Empty());
    if (auto& flow = flows_[index]) {
      for (auto& entry : flow->entries) {
        std::ranges::fill(entry, Empty());
      }
      std::ranges::fill(flow->reachable, 0);
    }
    if (facts.reachable) {
      work_.push_back(index);
      queued_[index] = 1;
    }
  }
  while (!work_.empty()) {
    const auto index = work_.back();
    work_.pop_back();
    queued_[index] = 0;
    Propagate(index);
  }
  // 只在调用不动点完成后发布逐指令证明；早期迭代的窄范围不能用于代码生成。
  for (std::size_t index = 0; index < facts_.size(); ++index) {
    Publish(index);
  }
  return facts_;
}

void IntegerRangeAnalysis::MergeCall(const ir::Instruction& instruction,
                                     std::span<const Range> state) {
  auto& callee = facts_[instruction.index];
  bool changed = !callee.reachable;
  callee.reachable = true;
  const auto has_state = !state.empty();
  for (std::size_t parameter = 0; parameter < callee.parameters.size(); ++parameter) {
    const auto value = has_state ? state[instruction.lhs + parameter] : Full();
    changed |=
        MergeRange(callee.parameters[parameter], value, thresholds_) == RangeChange::Expanded;
  }
  if (changed && queued_[instruction.index] == 0) {
    queued_[instruction.index] = 1;
    work_.push_back(instruction.index);
  }
}

void IntegerRangeAnalysis::Propagate(std::size_t index) {
  const auto& function = program_->functions[index];
  if (!ir::CanAnalyzeSynchronousFunction(function)) {
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::Call) {
        MergeCall(instruction, {});
      }
    }
    return;
  }
  if (!flows_[index]) {
    auto& flow = flows_[index].emplace();
    flow.graph = ir::BuildFlowGraph(function);
    flow.definitions = ir::FindRegisterDefinitions(function, accesses_[index]);
    const auto count = flow.graph.blocks.size();
    flow.entries.assign(count, State(function.register_count, Empty()));
    flow.reachable.resize(count);
    flow.queued.resize(count);
    flow.scratch.assign(function.register_count, Empty());
    flow.work.reserve(count);
  }
  Solve(index);
}

void IntegerRangeAnalysis::Solve(std::size_t function_index) {
  auto& flow = *flows_[function_index];
  const auto& function = program_->functions[function_index];
  const auto& input = facts_[function_index];
  if (flow.graph.blocks.empty()) {
    return;
  }
  auto& entry = flow.entries.front();
  for (std::size_t parameter = 0; parameter < input.parameters.size(); ++parameter) {
    MergeRange(entry[parameter], input.parameters[parameter], thresholds_);
  }
  flow.reachable.front() = 1;
  flow.work.push_back(0);
  flow.queued.front() = 1;
  auto& state = flow.scratch;
  const auto arrays = ArrayBits(function_index);
  while (!flow.work.empty()) {
    const auto index = flow.work.back();
    flow.work.pop_back();
    flow.queued[index] = 0;
    state = flow.entries[index];
    const auto& block = flow.graph.blocks[index];
    for (auto position = block.begin; position < block.end; ++position) {
      const auto& instruction = function.instructions[position];
      if (instruction.opcode == Opcode::Call) {
        MergeCall(instruction, state);
      }
      Transfer(function_index, position, state, arrays);
    }
    for (const auto successor : block.successors) {
      MergeEdge(function_index, block, state, successor);
    }
  }
}

void IntegerRangeAnalysis::MergeEdge(std::size_t function_index,
                                     const ir::BasicBlock& block,
                                     const State& state,
                                     std::size_t successor) {
  auto& flow = *flows_[function_index];
  const auto edge = RefineEdge(function_index, block, successor, state);
  if (edge && (edge->left.isEmptySet() || edge->right.isEmptySet())) {
    return;
  }
  bool changed = flow.reachable[successor] == 0;
  flow.reachable[successor] = 1;
  // 入口继承值也会在调用或回边迭代中扩张；不能只合并本块定义的寄存器。
  for (ir::RegisterId reg = 0; reg < state.size(); ++reg) {
    const auto* value = &state[reg];
    if (edge) {
      if (reg == edge->lhs) {
        value = &edge->left;
      } else if (reg == edge->rhs) {
        value = &edge->right;
      }
    }
    changed |=
        MergeRange(flow.entries[successor][reg], *value, thresholds_) == RangeChange::Expanded;
  }
  if (changed && flow.queued[successor] == 0) {
    flow.work.push_back(successor);
    flow.queued[successor] = 1;
  }
}

void IntegerRangeAnalysis::Transfer(std::size_t function_index,
                                    std::size_t position,
                                    State& state,
                                    std::span<const std::uint64_t> arrays) const {
  const auto& function = program_->functions[function_index];
  const auto& instruction = function.instructions[position];
  if (accesses_[function_index][position].defines_value) {
    state[instruction.dst] =
        IsInteger(function, instruction.dst) ? ResultRange(instruction, state, arrays) : Full();
  }
}

std::optional<IntegerRangeAnalysis::EdgeRanges> IntegerRangeAnalysis::RefineEdge(
    std::size_t function_index,
    const ir::BasicBlock& block,
    std::size_t successor,
    const State& state) const {
  const auto& function = program_->functions[function_index];
  const auto& flow = *flows_[function_index];
  const auto& graph = flow.graph;
  const auto position = block.end - 1;
  const auto& branch = function.instructions[position];
  if (branch.opcode != Opcode::JumpIfFalse) {
    return std::nullopt;
  }
  const auto& definition = flow.definitions[branch.lhs];
  const auto comparison_position = definition.last;
  if (definition.count != 1 || !comparison_position || *comparison_position >= position ||
      graph.instruction_blocks[*comparison_position] != graph.instruction_blocks[position] ||
      branch.index == position + 1) {
    return std::nullopt;
  }
  const auto& comparison = function.instructions[*comparison_position];
  auto predicate = IntegerComparison(comparison.opcode);
  if (!predicate) {
    return std::nullopt;
  }
  for (auto index = *comparison_position + 1; index < position; ++index) {
    const auto& instruction = function.instructions[index];
    if (accesses_[function_index][index].defines_value &&
        (instruction.dst == comparison.lhs || instruction.dst == comparison.rhs)) {
      return std::nullopt;
    }
  }
  if (branch.index < graph.instruction_blocks.size() &&
      graph.instruction_blocks[branch.index] == successor) {
    *predicate = llvm::CmpInst::getInversePredicate(*predicate);
  }
  const auto& left = state[comparison.lhs];
  const auto& right = state[comparison.rhs];
  auto refined_left =
      left.intersectWith(Range::makeAllowedICmpRegion(*predicate, right), Range::Signed);
  auto refined_right = right.intersectWith(
      Range::makeAllowedICmpRegion(llvm::CmpInst::getSwappedPredicate(*predicate), left),
      Range::Signed);
  if (comparison.lhs == comparison.rhs) {
    refined_left = refined_right;
  }
  return EdgeRanges{
      .lhs = comparison.lhs,
      .rhs = comparison.rhs,
      .left = std::move(refined_left),
      .right = std::move(refined_right),
  };
}

void IntegerRangeAnalysis::Publish(std::size_t index) {
  const auto& function = program_->functions[index];
  auto& result = facts_[index];
  const auto count = function.instructions.size();
  result.indices.assign(count, Full());
  result.writes.assign(count, Full());
  result.no_overflow.assign(count, 0);
  if (!flows_[index] || !result.reachable) {
    return;
  }
  auto& flow = *flows_[index];
  auto& state = flow.scratch;
  const auto arrays = ArrayBits(index);
  for (std::size_t block_index = 0; block_index < flow.graph.blocks.size(); ++block_index) {
    if (flow.reachable[block_index] == 0) {
      continue;
    }
    state = flow.entries[block_index];
    const auto& block = flow.graph.blocks[block_index];
    for (auto position = block.begin; position < block.end; ++position) {
      const auto& instruction = function.instructions[position];
      const auto opcode = instruction.opcode;
      result.no_overflow[position] = static_cast<std::uint8_t>(HasNoOverflow(instruction, state));
      if (opcode == Opcode::GetIndex || opcode == Opcode::SetIndex) {
        result.indices[position] =
            state[opcode == Opcode::GetIndex ? instruction.rhs : instruction.lhs];
      }
      if (opcode == Opcode::SetIndex) {
        result.writes[position] = state[instruction.rhs];
      }
      Transfer(index, position, state, arrays);
    }
  }
}

}  // namespace vela::compiler::native
