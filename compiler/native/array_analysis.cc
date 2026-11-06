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
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {
namespace {

constexpr auto kUnknownBits = std::numeric_limits<std::uint64_t>::max();

class ArrayAliases final {
 public:
  explicit ArrayAliases(const ir::Program& program)
      : initializers_(program.functions.size(), false),
        closed_(program.native_mode == NativeProgramMode::Closed) {
    auto count = program.global_initializers.size();
    for (const auto& function : program.functions) {
      offsets_.push_back(count);
      count += function.register_count;
    }
    parents_.resize(count);
    ranks_.resize(count);
    std::ranges::iota(parents_, std::size_t{0});
    if (closed_) {
      for (const auto initializer : program.global_initializers) {
        initializers_.at(initializer) = true;
      }
    }
    for (std::size_t index = 0; index < program.functions.size(); ++index) {
      for (const auto& instruction : program.functions.at(index).instructions) {
        const auto local = [&](ir::RegisterId reg) { return Node(index, reg); };
        if (instruction.opcode == Opcode::Move) {
          Unite(local(instruction.dst), local(instruction.lhs));
        } else if (closed_ && instruction.opcode == Opcode::GetGlobal) {
          Unite(local(instruction.dst), instruction.index);
        } else if (closed_ && instruction.opcode == Opcode::SetGlobal) {
          Unite(local(instruction.lhs), instruction.index);
        }
      }
    }
    if (closed_) {
      for (std::size_t global = 0; global < program.global_initializers.size(); ++global) {
        const auto index = program.global_initializers.at(global);
        for (const auto& instruction : program.functions.at(index).instructions) {
          if (instruction.opcode == Opcode::Return) {
            Unite(Node(index, instruction.dst), global);
          }
        }
      }
    }
  }

  [[nodiscard]] std::size_t size() const { return parents_.size(); }
  [[nodiscard]] std::size_t Node(std::size_t function, ir::RegisterId reg) const {
    return offsets_.at(function) + reg;
  }
  [[nodiscard]] bool IsGlobalAccess(std::size_t function, Opcode opcode) const {
    return closed_ && (opcode == Opcode::GetGlobal || opcode == Opcode::SetGlobal ||
                       (opcode == Opcode::Return && initializers_.at(function)));
  }

  std::size_t Find(std::size_t reg) {
    while (reg != parents_.at(reg)) {
      auto& parent = parents_.at(reg);
      parent = parents_.at(parent);
      reg = parent;
    }
    return reg;
  }

 private:
  void Unite(std::size_t first, std::size_t second) {
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

  std::vector<std::size_t> offsets_;
  std::vector<bool> initializers_;
  bool closed_;
  std::vector<std::size_t> parents_;
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
        case Builtin::ArrayClear:
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
                 std::size_t function,
                 ArrayAliases& aliases,
                 std::span<ArrayState> arrays,
                 std::span<const std::uint64_t> values) {
  if (instruction.opcode == Opcode::SetIndex) {
    arrays[aliases.Find(aliases.Node(function, instruction.dst))].bits |= values[instruction.rhs];
  } else if (instruction.opcode == Opcode::BuiltinCall &&
             instruction.index == static_cast<std::uint32_t>(Builtin::ArrayPush)) {
    arrays[aliases.Find(aliases.Node(function, instruction.lhs))].bits |=
        values[instruction.lhs + 1];
  }
}

std::vector<std::uint64_t> ArrayRegisterBits(const ir::Function& function,
                                             std::size_t function_index,
                                             ArrayAliases& aliases,
                                             std::span<const ArrayState> arrays) {
  const auto count = function.register_count;
  std::vector<std::uint64_t> bits(count, kUnknownBits);
  for (ir::RegisterId reg = 0; reg < count; ++reg) {
    const auto& array = arrays[aliases.Find(aliases.Node(function_index, reg))];
    if (array.local && !array.escaped) {
      bits.at(reg) = array.bits;
    }
  }
  return bits;
}

}  // namespace

std::vector<std::vector<std::uint64_t>> FindArrayElementBits(const ir::Program& program) {
  if (std::ranges::none_of(program.functions, [&](const auto& function) {
        return std::ranges::any_of(function.instructions, [&](const auto& instruction) {
          return HasLocalArrayDefinition(instruction, program);
        });
      })) {
    return std::vector<std::vector<std::uint64_t>>(program.functions.size());
  }
  ArrayAliases aliases(program);
  std::vector<ArrayState> arrays(aliases.size());
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions.at(index);
    const auto state = [&](ir::RegisterId reg) -> ArrayState& {
      return arrays.at(aliases.Find(aliases.Node(index, reg)));
    };
    for (ir::RegisterId reg = 0; reg < function.parameter_count; ++reg) {
      state(reg).escaped = true;
    }
    std::vector<ir::RegisterAccess> accesses;
    accesses.reserve(function.instructions.size());
    for (const auto& instruction : function.instructions) {
      const auto access = ir::AnalyzeRegisters(instruction, program.functions);
      accesses.push_back(access);
      const auto global_access = aliases.IsGlobalAccess(index, instruction.opcode);
      if (access.defines_value && instruction.opcode != Opcode::Move && !global_access) {
        auto& array = state(instruction.dst);
        if (HasLocalArrayDefinition(instruction, program)) {
          array.local = true;
        } else if (instruction.opcode != Opcode::LoadNull) {
          array.escaped = true;
        }
      }
      ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
        if (!global_access && !IsArrayAccess(instruction, reg)) {
          state(reg).escaped = true;
        }
      });
    }
    const auto values = IntegerBits(function, accesses);
    for (const auto& instruction : function.instructions) {
      RecordWrite(instruction, index, aliases, arrays, values);
    }
  }
  std::vector<std::vector<std::uint64_t>> result;
  result.reserve(program.functions.size());
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    result.push_back(ArrayRegisterBits(program.functions.at(index), index, aliases, arrays));
  }
  return result;
}

}  // namespace vela::compiler::native
