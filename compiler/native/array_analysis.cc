#include "compiler/native/array_analysis.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "base/layout_description.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {
namespace {

constexpr auto kUnknownBits = std::numeric_limits<std::uint64_t>::max();

class MoveAliases final {
 public:
  explicit MoveAliases(const ir::Function& function)
      : parents_(function.register_count), ranks_(function.register_count) {
    std::ranges::iota(parents_, ir::RegisterId{0});
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::Move) {
        Unite(instruction.dst, instruction.lhs);
      }
    }
  }

  ir::RegisterId Find(ir::RegisterId reg) {
    while (reg != parents_.at(reg)) {
      auto& parent = parents_.at(reg);
      parent = parents_.at(parent);
      reg = parent;
    }
    return reg;
  }

 private:
  void Unite(ir::RegisterId first, ir::RegisterId second) {
    auto left = Find(first);
    auto right = Find(second);
    if (left == right) {
      return;
    }
    if (ranks_.at(left) < ranks_.at(right)) {
      std::swap(left, right);
    }
    parents_.at(right) = left;
    if (ranks_.at(left) == ranks_.at(right)) {
      ++ranks_.at(left);
    }
  }

  std::vector<ir::RegisterId> parents_;
  std::vector<std::uint8_t> ranks_;
};

std::uint64_t ResultBits(const ir::Instruction& instruction, std::span<const std::uint64_t> bits) {
  switch (instruction.opcode) {
    case Opcode::LoadInt:
      return std::bit_cast<std::uint64_t>(instruction.immediate);
    case Opcode::Move:
      return bits[instruction.lhs];
    case Opcode::BitAndInt:
      return bits[instruction.lhs] & bits[instruction.rhs];
    case Opcode::BitOrInt:
    case Opcode::BitXorInt:
      return bits[instruction.lhs] | bits[instruction.rhs];
    default:
      return kUnknownBits;
  }
}

std::vector<std::uint64_t> IntegerBits(const ir::Function& function,
                                       std::span<const ir::RegisterAccess> accesses) {
  std::vector<std::uint64_t> bits(function.register_count);
  std::fill_n(bits.begin(), function.parameter_count, kUnknownBits);
  std::vector<std::vector<std::size_t>> users(function.register_count);
  std::vector<std::size_t> pending;
  std::vector<std::uint8_t> queued(function.instructions.size());
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& access = accesses[index];
    if (!access.defines_value) {
      continue;
    }
    pending.push_back(index);
    queued.at(index) = 1;
    ir::ForEachUse(function.instructions.at(index), access,
                   [&](ir::RegisterId reg) { users.at(reg).push_back(index); });
  }
  // 位集合从空集单调增长；每个位最多加入一次。反向使用表避免按函数大小反复全表扫描。
  while (!pending.empty()) {
    const auto index = pending.back();
    pending.pop_back();
    queued.at(index) = 0;
    const auto& instruction = function.instructions.at(index);
    auto& current = bits.at(instruction.dst);
    const auto combined = current | ResultBits(instruction, bits);
    if (current == combined) {
      continue;
    }
    current = combined;
    for (const auto user : users.at(instruction.dst)) {
      auto& scheduled = queued.at(user);
      if (scheduled == 0) {
        scheduled = 1;
        pending.push_back(user);
      }
    }
  }
  return bits;
}

bool IsIntegerArray(const ir::Instruction& instruction, const ir::Program& program) {
  const auto& arrays = program.layouts.arrays;
  const auto& element = arrays.at(program.layouts.array_types.at(instruction.index)).element;
  return element.kind == layout::Type::Kind::Integer && !element.nullable;
}

bool HasLocalArrayDefinition(const ir::Instruction& instruction, const ir::Program& program) {
  return (instruction.opcode == Opcode::NewArray ||
          instruction.opcode == Opcode::NewDynamicArray) &&
         IsIntegerArray(instruction, program);
}

bool IsArrayAccess(const ir::Instruction& instruction, ir::RegisterId reg) {
  switch (instruction.opcode) {
    case Opcode::Move:
      return true;
    case Opcode::GetIndex:
      return reg == instruction.lhs;
    case Opcode::SetIndex:
      return reg == instruction.dst;
    case Opcode::BuiltinCall:
      if (reg != instruction.lhs) {
        return false;
      }
      switch (static_cast<Builtin>(instruction.index)) {
        case Builtin::ArrayLength:
        case Builtin::ArrayCapacity:
        case Builtin::ArrayMutation:
        case Builtin::ArrayCheckMutation:
        case Builtin::ArrayReserve:
        case Builtin::ArrayPush:
          return true;
        default:
          return false;
      }
    default:
      return false;
  }
}

struct ArrayState final {
  bool local = false;
  bool escaped = false;
  std::uint64_t bits = 0;
};

void RecordWrite(const ir::Instruction& instruction,
                 MoveAliases& aliases,
                 std::span<ArrayState> arrays,
                 std::span<const std::uint64_t> values) {
  if (instruction.opcode == Opcode::SetIndex) {
    arrays[aliases.Find(instruction.dst)].bits |= values[instruction.rhs];
  } else if (instruction.opcode == Opcode::BuiltinCall &&
             instruction.index == static_cast<std::uint32_t>(Builtin::ArrayPush)) {
    arrays[aliases.Find(instruction.lhs)].bits |= values[instruction.lhs + 1];
  }
}

}  // namespace

std::vector<std::uint64_t> FindPrivateArrayElementBits(const ir::Function& function,
                                                       const ir::Program& program) {
  if (std::ranges::none_of(function.instructions, [&](const auto& instruction) {
        return HasLocalArrayDefinition(instruction, program);
      })) {
    return {};
  }
  MoveAliases aliases(function);
  std::vector<ArrayState> arrays(function.register_count);
  for (ir::RegisterId reg = 0; reg < function.parameter_count; ++reg) {
    arrays.at(aliases.Find(reg)).escaped = true;
  }
  std::vector<ir::RegisterAccess> accesses;
  accesses.reserve(function.instructions.size());
  for (const auto& instruction : function.instructions) {
    const auto access = ir::AnalyzeRegisters(instruction, program.functions);
    accesses.push_back(access);
    if (access.defines_value && instruction.opcode != Opcode::Move) {
      auto& array = arrays.at(aliases.Find(instruction.dst));
      if (HasLocalArrayDefinition(instruction, program)) {
        array.local = true;
      } else if (instruction.opcode != Opcode::LoadNull) {
        array.escaped = true;
      }
    }
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
      if (!IsArrayAccess(instruction, reg)) {
        arrays.at(aliases.Find(reg)).escaped = true;
      }
    });
  }
  const auto values = IntegerBits(function, accesses);
  for (const auto& instruction : function.instructions) {
    RecordWrite(instruction, aliases, arrays, values);
  }
  std::vector<std::uint64_t> result(function.register_count, kUnknownBits);
  for (ir::RegisterId reg = 0; reg < function.register_count; ++reg) {
    const auto& array = arrays.at(aliases.Find(reg));
    if (array.local && !array.escaped) {
      result.at(reg) = array.bits;
    }
  }
  return result;
}

}  // namespace vela::compiler::native
