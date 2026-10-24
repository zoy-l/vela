#include "compiler/register_allocator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "base/execution_limits.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler {
namespace {

constexpr auto kNoPosition = std::numeric_limits<std::size_t>::max();

struct Interval final {
  std::size_t begin = kNoPosition;
  std::size_t end = 0;
};

struct Allocation final {
  ir::RegisterId first = 0;
  ir::RegisterId count = 1;
  Interval interval;
  ir::RegisterId physical = 0;
};

void Touch(Interval& interval, std::size_t position) {
  interval.begin = std::min(interval.begin, position);
  interval.end = std::max(interval.end, position);
}

auto LiveIntervals(const ir::Function& function, std::span<const ir::Function> functions)
    -> std::vector<Interval> {
  std::vector<Interval> intervals(function.register_count);
  for (std::uint32_t parameter = 0; parameter < function.parameter_count; ++parameter) {
    Touch(intervals.at(parameter), 0);
  }
  const auto graph = ir::BuildFlowGraph(function);
  const auto blocks = ir::AnalyzeLiveness(function, graph, functions);
  for (std::size_t block_index = 0; block_index < graph.blocks.size(); ++block_index) {
    const auto& block = graph.blocks.at(block_index);
    const auto& live = blocks.at(block_index);
    for (const auto reg : live.live_in) {
      Touch(intervals.at(reg), (block.begin * 2) + 1);
    }
    for (const auto reg : live.live_out) {
      Touch(intervals.at(reg), (block.end * 2) + 1);
    }
  }
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    const auto access = ir::AnalyzeRegisters(instruction, functions);
    // 同一指令先读后写；死亡输入可与结果复用槽位，包括分配指令的目标。
    ir::ForEachUse(instruction, access,
                   [&](ir::RegisterId reg) -> void { Touch(intervals.at(reg), (index * 2) + 1); });
    if (access.defines_value) {
      Touch(intervals.at(instruction.dst), (index * 2) + 2);
    }
  }
  return intervals;
}

auto BuildAllocations(const ir::Function& function,
                      std::span<const ir::Function> functions,
                      std::span<const Interval> intervals) -> std::vector<Allocation> {
  std::vector<Allocation> windows;
  for (const auto& instruction : function.instructions) {
    const auto access = ir::AnalyzeRegisters(instruction, functions);
    if (access.argument_count != 0) {
      windows.push_back({.first = instruction.lhs, .count = access.argument_count, .interval = {}});
    }
  }
  std::ranges::sort(windows, {}, &Allocation::first);
  std::vector<Allocation> merged;
  for (const auto& window : windows) {
    if (!merged.empty() && window.first < merged.back().first + merged.back().count) {
      auto& previous = merged.back();
      previous.count =
          std::max(previous.first + previous.count, window.first + window.count) - previous.first;
    } else {
      merged.push_back(window);
    }
  }
  std::vector<Allocation> allocations;
  std::size_t window_index = 0;
  for (ir::RegisterId reg = 0; reg < intervals.size();) {
    Allocation allocation{.first = reg, .interval = {}};
    if (window_index < merged.size() && merged.at(window_index).first == reg) {
      allocation = merged.at(window_index++);
    }
    for (ir::RegisterId offset = 0; offset < allocation.count; ++offset) {
      const auto& interval = intervals.subspan(reg + offset).front();
      allocation.interval.begin = std::min(allocation.interval.begin, interval.begin);
      allocation.interval.end = std::max(allocation.interval.end, interval.end);
    }
    if (allocation.interval.begin != kNoPosition) {
      allocations.push_back(allocation);
    }
    reg += allocation.count;
  }
  std::ranges::sort(allocations, [](const auto& left, const auto& right) -> bool {
    if (left.interval.begin != right.interval.begin) {
      return left.interval.begin < right.interval.begin;
    }
    return left.first < right.first;
  });
  return allocations;
}

auto FindWindow(std::span<const std::uint8_t> occupied, std::uint32_t count)
    -> std::optional<ir::RegisterId> {
  std::uint32_t run = 0;
  for (std::uint32_t index = 0; index < occupied.size(); ++index) {
    run = occupied.subspan(index).front() == 0 ? run + 1 : 0;
    if (run == count) {
      return index + 1 - count;
    }
  }
  return std::nullopt;
}

auto AssignWindows(std::span<Allocation> allocations, std::uint32_t parameter_count)
    -> std::optional<std::size_t> {
  std::vector<std::uint8_t> occupied(kMaxFrameRegisters, 0);
  std::vector<std::size_t> active;
  for (std::size_t index = 0; index < allocations.size(); ++index) {
    auto& allocation = allocations.subspan(index).front();
    std::erase_if(active, [&](std::size_t previous_index) -> bool {
      const auto& previous = allocations.subspan(previous_index).front();
      if (previous.interval.end >= allocation.interval.begin) {
        return false;
      }
      std::ranges::fill(std::span(occupied).subspan(previous.physical, previous.count), 0);
      return true;
    });
    const auto physical = allocation.first < parameter_count
                              ? std::optional<ir::RegisterId>(allocation.first)
                              : FindWindow(occupied, allocation.count);
    if (!physical || *physical + allocation.count > kMaxFrameRegisters) {
      return allocation.interval.begin;
    }
    allocation.physical = *physical;
    std::ranges::fill(std::span(occupied).subspan(*physical, allocation.count), 1);
    active.push_back(index);
  }
  return std::nullopt;
}

void Rewrite(ir::Function& function,
             std::span<const ir::Function> functions,
             std::span<const Allocation> allocations) {
  std::vector<ir::RegisterId> mapping(function.register_count, 0);
  std::uint32_t count = function.parameter_count;
  for (const auto& allocation : allocations) {
    for (ir::RegisterId offset = 0; offset < allocation.count; ++offset) {
      mapping.at(allocation.first + offset) = allocation.physical + offset;
    }
    count = std::max(count, allocation.physical + allocation.count);
  }
  for (auto& instruction : function.instructions) {
    const auto access = ir::AnalyzeRegisters(instruction, functions);
    const auto original = instruction;
    for (const auto operand : std::span(access.uses).first(access.use_count)) {
      ir::WriteOperand(instruction, operand, mapping.at(ir::ReadOperand(original, operand)));
    }
    if (access.defines_value) {
      instruction.dst = mapping.at(original.dst);
    }
    if (original.opcode == ir::Opcode::Call || original.opcode == ir::Opcode::BuiltinCall ||
        original.opcode == ir::Opcode::StringInterpolate) {
      instruction.lhs = access.argument_count != 0 ? mapping.at(original.lhs) : 0;
    }
  }
  function.register_count = std::max(count, std::uint32_t{1});
}

void RemoveSelfMoves(ir::Function& function) {
  std::vector<std::uint8_t> keep(function.instructions.size(), 1);
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    if (instruction.opcode == ir::Opcode::Move && instruction.dst == instruction.lhs) {
      keep.at(index) = 0;
    }
  }
  ir::CompactInstructions(function, keep);
}

}  // namespace

auto AllocateRegisters(ir::Function& function, std::span<const ir::Function> functions)
    -> std::optional<frontend::Diagnostic> {
  if (function.parameter_count > kMaxFrameRegisters) {
    return frontend::Diagnostic{
        .location = function.instructions.front().location,
        .message = "function parameter count exceeds the VM limit of 256",
        .source_path = {},
    };
  }
  const auto intervals = LiveIntervals(function, functions);
  auto allocations = BuildAllocations(function, functions, intervals);
  const auto failure = AssignWindows(allocations, function.parameter_count);
  if (failure) {
    const auto position = *failure == 0 ? 0 : (*failure - 1) / 2;
    const auto instruction = std::min(position, function.instructions.size() - 1);
    return frontend::Diagnostic{
        .location = function.instructions.at(instruction).location,
        .message =
            "function requires more than 256 register slots; reduce simultaneous live values or "
            "call arguments",
        .source_path = {},
    };
  }
  Rewrite(function, functions, allocations);
  RemoveSelfMoves(function);
  return std::nullopt;
}

}  // namespace vela::compiler
