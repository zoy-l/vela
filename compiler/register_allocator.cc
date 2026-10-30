#include "compiler/register_allocator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/execution_limits.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
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

std::vector<Interval> LiveIntervals(const ir::Function& function,
                                    std::span<const ir::Function> functions) {
  std::vector<Interval> intervals(function.register_count);
  for (std::uint32_t parameter = 0; parameter < function.parameter_count; ++parameter) {
    Touch(intervals.at(parameter), 0);
  }
  const auto graph = ir::BuildFlowGraph(function);
  const auto blocks = ir::AnalyzeLiveness(function, graph, functions);
  for (std::size_t block_index = 0; block_index < graph.blocks.size(); ++block_index) {
    const auto& block = graph.blocks.at(block_index);
    const auto& live = blocks.at(block_index);
    live.live_in.ForEach(
        [&](ir::RegisterId reg) { Touch(intervals.at(reg), (block.begin * 2) + 1); });
    live.live_out.ForEach(
        [&](ir::RegisterId reg) { Touch(intervals.at(reg), (block.end * 2) + 1); });
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

std::vector<Allocation> BuildAllocations(const ir::Function& function,
                                         std::span<const ir::Function> functions,
                                         std::span<const Interval> intervals) {
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

std::optional<ir::RegisterId> FindWindow(std::span<const std::uint8_t> occupied,
                                         std::uint32_t count) {
  std::uint32_t run = 0;
  for (std::uint32_t index = 0; index < occupied.size(); ++index) {
    run = occupied.subspan(index).front() == 0 ? run + 1 : 0;
    if (run == count) {
      return index + 1 - count;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> AssignWindows(std::span<Allocation> allocations,
                                         std::uint32_t parameter_count) {
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
    if (original.opcode == ir::Opcode::Call || original.opcode == ir::Opcode::CallInterface ||
        original.opcode == ir::Opcode::CallIndirect || original.opcode == ir::Opcode::BuiltinCall ||
        original.opcode == ir::Opcode::AsyncCall ||
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

bool MayHoldHeapReference(const frontend::TypeDescriptor& type) {
  using Kind = frontend::TypeDescriptor::Kind;
  return type.kind != Kind::Invalid && type.kind != Kind::Null && type.kind != Kind::Void &&
         type.kind != Kind::Bool && type.kind != Kind::Int && type.kind != Kind::Double;
}

ir::RegisterSet ManagedSlots(const ir::Function& function,
                             std::span<const Allocation> allocations) {
  ir::RegisterSet managed(function.register_count);
  for (const auto& allocation : allocations) {
    for (ir::RegisterId offset = 0; offset < allocation.count; ++offset) {
      if (MayHoldHeapReference(function.value_types.at(allocation.first + offset))) {
        managed.Insert(allocation.physical + offset);
      }
    }
  }
  return managed;
}

void ClearDeadSuspendedRegisters(ir::Function& function,
                                 std::span<const ir::Function> functions,
                                 const ir::RegisterSet& managed) {
  if (managed.IsEmpty() || std::ranges::none_of(function.instructions, [](const auto& instruction) {
        return instruction.opcode == ir::Opcode::Await;
      })) {
    return;
  }
  const auto graph = ir::BuildFlowGraph(function);
  const auto liveness = ir::AnalyzeLiveness(function, graph, functions);
  std::vector<std::vector<ir::RegisterId>> clear(function.instructions.size());
  for (std::size_t block_index = 0; block_index < graph.blocks.size(); ++block_index) {
    const auto& block = graph.blocks.at(block_index);
    // CFG 在挂起指令之后切块；Await 只能位于块尾，清理只需要它的输入活性。
    const auto index = block.end - 1;
    const auto& instruction = function.instructions.at(index);
    if (instruction.opcode != ir::Opcode::Await) {
      continue;
    }
    auto live = liveness.at(block_index).live_out;
    const auto access = ir::AnalyzeRegisters(instruction, functions);
    ir::TransferLiveness(function, graph, liveness, instruction, access, live);
    auto dead = managed;
    dead.Subtract(live);
    auto& registers = clear.at(index);
    dead.ForEach([&registers](ir::RegisterId reg) { registers.push_back(reg); });
  }
  std::vector<ir::Instruction> code;
  std::vector<std::uint32_t> remap(function.instructions.size() + 1);
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    remap.at(index) = static_cast<std::uint32_t>(code.size());
    const auto& instruction = function.instructions.at(index);
    for (const auto reg : clear.at(index)) {
      code.push_back(
          {.opcode = ir::Opcode::LoadNull, .dst = reg, .location = instruction.location});
    }
    code.push_back(instruction);
  }
  remap.back() = static_cast<std::uint32_t>(code.size());
  for (auto& instruction : code) {
    if (instruction.opcode == ir::Opcode::Jump || instruction.opcode == ir::Opcode::JumpIfFalse ||
        instruction.opcode == ir::Opcode::JumpIfNull) {
      instruction.index = remap.at(instruction.index);
    }
  }
  for (auto& handler : function.handlers) {
    handler.target = remap.at(handler.target);
  }
  function.instructions = std::move(code);
}

}  // namespace

std::optional<frontend::Diagnostic> AllocateRegisters(ir::Function& function,
                                                      std::span<const ir::Function> functions) {
  if (function.parameter_count > kMaxFrameRegisters) {
    return frontend::Diagnostic{
        .location = function.location,
        .message = "function parameter count exceeds the VM limit of 256",
        .source_path = {},
    };
  }
  if (!function.native_symbol.empty()) {
    return std::nullopt;
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
  if (function.is_async) {
    // 物理槽会在不同生命期保存不同类型；不能用最后一个虚拟值的类型覆盖引用事实。
    // VM 中 enum 也可能是盒装对象，须按可能持有引用处理。
    const auto managed = ManagedSlots(function, allocations);
    ClearDeadSuspendedRegisters(function, functions, managed);
  }
  function.value_types.clear();
  return std::nullopt;
}

}  // namespace vela::compiler
