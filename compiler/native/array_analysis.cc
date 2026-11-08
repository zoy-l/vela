#include "compiler/native/array_analysis.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/array_capacity_analysis.h"
#include "compiler/native/integer_range_analysis.h"

namespace vela::compiler::native {
namespace {

enum class ArrayCallDomain : std::uint8_t { Generic, DirectCalls };
enum class ArrayAccessUse : std::uint8_t { Both, Elements, FixedStorage };

class ArrayAliases final {
 public:
  explicit ArrayAliases(const ir::Program& program, ArrayCallDomain domain)
      : initializers_(program.functions.size(), false),
        closed_(program.native_mode == NativeProgramMode::Closed),
        calls_(domain == ArrayCallDomain::DirectCalls) {
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
        MergeInstruction(index, instruction, program);
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
  [[nodiscard]] const ir::Function* FindArrayCallee(const ir::Instruction& instruction,
                                                    const ir::Program& program) const {
    if (!calls_ || instruction.opcode != Opcode::Call) {
      return nullptr;
    }
    const auto& callee = program.functions[instruction.index];
    // 异常处理不改变同步实参别名；handler 内的未知使用仍按逃逸处理。
    // 数值 CFG 分析不跨异常边，不能把它的资格谓词用于此处的别名合并。
    return callee.native_symbol.empty() && !callee.is_async ? &callee : nullptr;
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
  void MergeInstruction(std::size_t index,
                        const ir::Instruction& instruction,
                        const ir::Program& program) {
    const auto local = [&](ir::RegisterId reg) { return Node(index, reg); };
    if (instruction.opcode == Opcode::Move) {
      Unite(local(instruction.dst), local(instruction.lhs));
    } else if (closed_ && instruction.opcode == Opcode::GetGlobal) {
      Unite(local(instruction.dst), instruction.index);
    } else if (closed_ && instruction.opcode == Opcode::SetGlobal) {
      Unite(local(instruction.lhs), instruction.index);
    } else if (const auto* callee = FindArrayCallee(instruction, program)) {
      for (ir::RegisterId parameter = 0; parameter < callee->parameter_count; ++parameter) {
        Unite(local(instruction.lhs + parameter), Node(instruction.index, parameter));
      }
    }
  }

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
  bool calls_;
  std::vector<std::size_t> parents_;
  std::vector<std::uint8_t> ranks_;
};

struct ArrayFunctionFlow final {
  std::vector<std::vector<std::size_t>> users;
  std::vector<std::size_t> definitions;
  std::vector<std::size_t> writes;
  std::vector<std::uint64_t> bits;
  std::vector<std::size_t> work;
  std::vector<std::uint8_t> queued;
};

std::span<const std::uint64_t> IntegerBits(const ir::Function& function,
                                           ArrayFunctionFlow& flow,
                                           std::span<const std::uint64_t> arrays) {
  auto& bits = flow.bits;
  bits.assign(function.register_count, 0);
  std::fill_n(bits.begin(), function.parameter_count, kUnknownArrayElementBits);
  const auto result_bits = [&](const ir::Instruction& instruction) {
    switch (instruction.opcode) {
      case Opcode::LoadInt:
        return std::bit_cast<std::uint64_t>(instruction.immediate);
      case Opcode::Move:
        return bits[instruction.lhs];
      case Opcode::GetIndex:
        return arrays[instruction.lhs];
      case Opcode::BitAndInt:
        return bits[instruction.lhs] & bits[instruction.rhs];
      case Opcode::BitOrInt:
      case Opcode::BitXorInt:
        return bits[instruction.lhs] | bits[instruction.rhs];
      default:
        return kUnknownArrayElementBits;
    }
  };
  auto& pending = flow.work;
  auto& queued = flow.queued;
  pending = flow.definitions;
  queued.assign(function.instructions.size(), 0);
  for (const auto index : pending) {
    queued[index] = 1;
  }
  // 位集合从空集单调增长；每个位最多加入一次。反向使用表避免按函数大小反复全表扫描。
  while (!pending.empty()) {
    const auto index = pending.back();
    pending.pop_back();
    queued.at(index) = 0;
    const auto& instruction = function.instructions.at(index);
    auto& current = bits.at(instruction.dst);
    const auto combined = current | result_bits(instruction);
    if (current == combined) {
      continue;
    }
    current = combined;
    for (const auto user : flow.users[instruction.dst]) {
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

struct ArrayAccess final {
  ir::RegisterId reg;
  ArrayAccessUse use;
};

std::optional<ArrayAccess> ClassifyArrayAccess(const ir::Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::Move:
    case Opcode::GetIndex:
      return ArrayAccess{.reg = instruction.lhs, .use = ArrayAccessUse::Both};
    case Opcode::SetIndex:
      return ArrayAccess{.reg = instruction.dst, .use = ArrayAccessUse::Both};
    case Opcode::BuiltinCall:
      switch (static_cast<Builtin>(instruction.index)) {
        case Builtin::ArrayLength:
        case Builtin::ArrayCapacity:
        case Builtin::ArrayMutation:
        case Builtin::ArrayCheckMutation:
          return ArrayAccess{.reg = instruction.lhs, .use = ArrayAccessUse::Both};
        case Builtin::ArrayReserve:
        case Builtin::ArrayClear:
        case Builtin::ArrayPush:
          return ArrayAccess{.reg = instruction.lhs, .use = ArrayAccessUse::Elements};
        case Builtin::ArraySlice:
          return ArrayAccess{.reg = instruction.lhs, .use = ArrayAccessUse::FixedStorage};
        default:
          return std::nullopt;
      }
    default:
      return std::nullopt;
  }
}

struct ArrayElementState final {
  bool local = false;
  bool escaped = false;
  std::uint64_t bits = 0;
};

struct ArrayStorageState final {
  bool local = false;
  bool escaped = false;
  std::optional<std::uint32_t> minimum_length;
};

struct ArrayStorageFacts final {
  std::vector<std::vector<std::uint8_t>> fixed;
  std::vector<std::vector<std::optional<std::uint32_t>>> minimum_lengths;
};

struct ArrayDomainFacts final {
  std::vector<std::vector<std::uint64_t>> bits;
  ArrayStorageFacts storage;
};

void RecordArrayDefinition(ArrayStorageState& array, const ir::Instruction& instruction);
ArrayStorageFacts CollectArrayStorageFacts(const ir::Program& program,
                                           ArrayAliases& aliases,
                                           std::span<const ArrayStorageState> states,
                                           ArrayCallDomain domain);

std::vector<std::uint64_t> ArrayRegisterBits(const ir::Function& function,
                                             std::size_t function_index,
                                             ArrayAliases& aliases,
                                             std::span<const ArrayElementState> arrays) {
  const auto count = function.register_count;
  std::vector<std::uint64_t> bits(count, kUnknownArrayElementBits);
  for (ir::RegisterId reg = 0; reg < count; ++reg) {
    const auto& array = arrays[aliases.Find(aliases.Node(function_index, reg))];
    if (array.local && !array.escaped) {
      bits.at(reg) = array.bits;
    }
  }
  return bits;
}

bool IsArrayWrite(const ir::Instruction& instruction) {
  return instruction.opcode == Opcode::SetIndex ||
         ir::IsBuiltinCall(instruction, Builtin::ArrayPush);
}

class ArrayDomainAnalysis final {
 public:
  explicit ArrayDomainAnalysis(const ir::Program& program,
                               ArrayCallDomain domain,
                               std::span<const std::vector<ir::RegisterAccess>> accesses,
                               ArrayAnalysisMode mode,
                               std::span<const std::uint8_t> roots = {},
                               std::span<const IntegerRangeFacts> ranges = {})
      : program_(&program),
        domain_(domain),
        aliases_(program, domain),
        roots_(roots),
        ranges_(ranges),
        arrays_(aliases_.size()),
        track_storage_(mode != ArrayAnalysisMode::Elements),
        storage_(track_storage_ ? aliases_.size() : 0),
        accesses_(accesses),
        readers_(aliases_.size()),
        flows_(program.functions.size()),
        queued_(program.functions.size()) {
    for (std::size_t index = 0; index < program.functions.size(); ++index) {
      InitializeFunction(index);
    }
    for (auto& readers : readers_) {
      std::ranges::sort(readers);
      const auto duplicates = std::ranges::unique(readers);
      readers.erase(duplicates.begin(), duplicates.end());
    }
  }

  ArrayDomainFacts Run() {
    // 数组之间的复制共用同一单调位集合；未知输入先污染别名组，再经读取依赖传播。
    // 只有发生增长的组才唤醒其读者，避免逐轮扫描整个程序。
    while (!pending_.empty()) {
      const auto index = pending_.back();
      pending_.pop_back();
      queued_[index] = 0;
      PropagateFunction(index);
    }
    std::vector<std::vector<std::uint64_t>> result;
    result.reserve(program_->functions.size());
    for (std::size_t index = 0; index < program_->functions.size(); ++index) {
      result.push_back(ArrayRegisterBits(program_->functions[index], index, aliases_, arrays_));
    }
    return {
        .bits = std::move(result),
        .storage = track_storage_ ? CollectArrayStorageFacts(*program_, aliases_, storage_, domain_)
                                  : ArrayStorageFacts{},
    };
  }

 private:
  std::size_t Group(std::size_t function, ir::RegisterId reg) {
    return aliases_.Find(aliases_.Node(function, reg));
  }

  void InitializeDefinition(std::size_t index, const ir::Instruction& instruction) {
    const auto group = Group(index, instruction.dst);
    if (track_storage_) {
      RecordArrayDefinition(storage_[group], instruction);
    }
    if (instruction.opcode != Opcode::Move && !aliases_.IsGlobalAccess(index, instruction.opcode)) {
      auto& array = arrays_[group];
      const auto local = HasLocalArrayDefinition(instruction, *program_);
      array.local |= local;
      array.escaped |= !local && instruction.opcode != Opcode::LoadNull;
    }
  }

  void InitializeFunction(std::size_t index) {
    const auto& function = program_->functions[index];
    auto& flow = flows_[index];
    for (std::size_t position = 0; position < function.instructions.size(); ++position) {
      if (IsArrayWrite(function.instructions[position])) {
        flow.writes.push_back(position);
      }
    }
    const auto writes = !flow.writes.empty();
    if (writes) {
      flow.users.resize(function.register_count);
      pending_.push_back(index);
      queued_[index] = 1;
    }
    if (roots_.empty() || roots_[index] != 0) {
      for (ir::RegisterId reg = 0; reg < function.parameter_count; ++reg) {
        const auto group = Group(index, reg);
        arrays_[group].escaped = true;
        if (track_storage_) {
          storage_[group].escaped = true;
        }
      }
    }
    for (std::size_t position = 0; position < function.instructions.size(); ++position) {
      InitializeInstruction(index, position);
    }
  }

  void InitializeInstruction(std::size_t index, std::size_t position) {
    const auto& instruction = program_->functions[index].instructions[position];
    const auto& access = accesses_[index][position];
    auto& flow = flows_[index];
    const auto writes = !flow.writes.empty();
    if (writes && instruction.opcode == Opcode::GetIndex) {
      readers_[Group(index, instruction.lhs)].push_back(index);
    }
    const auto global_access = aliases_.IsGlobalAccess(index, instruction.opcode);
    const auto call_access = aliases_.FindArrayCallee(instruction, *program_) != nullptr;
    if (access.defines_value) {
      InitializeDefinition(index, instruction);
      if (writes) {
        flow.definitions.push_back(position);
      }
    }
    const auto array_access = ClassifyArrayAccess(instruction);
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
      if (writes && access.defines_value) {
        flow.users[reg].push_back(position);
      }
      const auto group = Group(index, reg);
      const auto array_use = array_access && array_access->reg == reg
                                 ? array_access->use
                                 : std::optional<ArrayAccessUse>{};
      if ((domain_ == ArrayCallDomain::DirectCalls && global_access) ||
          (!global_access && !call_access &&
           (!array_use || *array_use == ArrayAccessUse::FixedStorage))) {
        arrays_[group].escaped = true;
      }
      if (track_storage_ && !call_access &&
          (!array_use || *array_use == ArrayAccessUse::Elements)) {
        storage_[group].escaped = true;
      }
    });
  }

  void MergeBits(ArrayElementState& array,
                 std::uint64_t added,
                 std::span<const std::size_t> readers) {
    auto& bits = array.bits;
    const auto combined = bits | added;
    if (bits == combined) {
      return;
    }
    bits = combined;
    for (const auto reader : readers) {
      if (queued_[reader] == 0) {
        queued_[reader] = 1;
        pending_.push_back(reader);
      }
    }
  }

  void PropagateFunction(std::size_t index) {
    const auto& function = program_->functions[index];
    const auto array_bits = ArrayRegisterBits(function, index, aliases_, arrays_);
    auto& flow = flows_[index];
    const auto values = IntegerBits(function, flow, array_bits);
    for (const auto position : flow.writes) {
      const auto& instruction = function.instructions[position];
      const auto indexed = instruction.opcode == Opcode::SetIndex;
      const auto array = indexed ? instruction.dst : instruction.lhs;
      const auto value = indexed ? instruction.rhs : instruction.lhs + 1;
      const auto group = Group(index, array);
      auto bits = values[value];
      if (!ranges_.empty() && indexed) {
        const auto& range = ranges_[index].writes[position];
        if (!range.isEmptySet() && range.getSignedMin().isNonNegative()) {
          const auto maximum = range.getUnsignedMax().getZExtValue();
          const auto mask =
              maximum == 0 ? 0 : kUnknownArrayElementBits >> std::countl_zero(maximum);
          bits &= mask;
        }
      }
      MergeBits(arrays_[group], bits, readers_[group]);
    }
  }

  const ir::Program* program_;
  ArrayCallDomain domain_;
  ArrayAliases aliases_;
  std::span<const std::uint8_t> roots_;
  std::span<const IntegerRangeFacts> ranges_;
  std::vector<ArrayElementState> arrays_;
  bool track_storage_;
  std::vector<ArrayStorageState> storage_;
  std::span<const std::vector<ir::RegisterAccess>> accesses_;
  std::vector<std::vector<std::size_t>> readers_;
  std::vector<ArrayFunctionFlow> flows_;
  std::vector<std::size_t> pending_;
  std::vector<std::uint8_t> queued_;
};

void RecordArrayDefinition(ArrayStorageState& array, const ir::Instruction& instruction) {
  // slice 创建独立固定存储，但会读取来源元素；元素位分析不能把它当作已知 int[] 构造。
  const auto allocation = instruction.opcode == Opcode::NewArray ||
                          instruction.opcode == Opcode::NewDynamicArray ||
                          ir::IsBuiltinCall(instruction, Builtin::ArraySlice);
  if (!allocation) {
    array.escaped |= instruction.opcode != Opcode::Move;
    return;
  }
  const auto length = instruction.opcode == Opcode::NewArray
                          ? std::optional(static_cast<std::uint32_t>(instruction.immediate))
                          : std::nullopt;
  if (!array.local) {
    array.minimum_length = length;
  } else if (array.minimum_length && length) {
    array.minimum_length = std::min(*array.minimum_length, *length);
  } else {
    array.minimum_length.reset();
  }
  array.local = true;
}

ArrayStorageFacts CollectArrayStorageFacts(const ir::Program& program,
                                           ArrayAliases& aliases,
                                           std::span<const ArrayStorageState> states,
                                           ArrayCallDomain domain) {
  const auto track_lengths = domain == ArrayCallDomain::DirectCalls;
  ArrayStorageFacts result{
      .fixed = std::vector<std::vector<std::uint8_t>>(program.functions.size()),
      .minimum_lengths = std::vector<std::vector<std::optional<std::uint32_t>>>(
          track_lengths ? program.functions.size() : 0),
  };
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions[index];
    auto& fixed = result.fixed[index];
    fixed.resize(function.register_count);
    std::span<std::optional<std::uint32_t>> lengths;
    if (track_lengths) {
      auto& function_lengths = result.minimum_lengths[index];
      function_lengths.resize(function.register_count);
      lengths = function_lengths;
    }
    for (ir::RegisterId reg = 0; reg < function.register_count; ++reg) {
      const auto& array = states[aliases.Find(aliases.Node(index, reg))];
      const auto fixed_storage =
          function.value_types[reg].kind == frontend::TypeDescriptor::Kind::Array && array.local &&
          !array.escaped;
      fixed[reg] = static_cast<std::uint8_t>(fixed_storage);
      if (track_lengths && fixed_storage) {
        lengths[reg] = array.minimum_length;
      }
    }
  }
  return result;
}

std::vector<ArrayAccessFacts> PackArrayAccessFacts(ArrayDomainFacts domain) {
  auto& bits = domain.bits;
  auto& fixed = domain.storage.fixed;
  const auto has_fixed_storage = !fixed.empty();
  std::vector<ArrayAccessFacts> facts;
  facts.reserve(bits.size());
  for (std::size_t index = 0; index < bits.size(); ++index) {
    facts.push_back({
        .element_bits = std::move(bits[index]),
        .fixed_storage = has_fixed_storage ? std::move(fixed[index]) : std::vector<std::uint8_t>{},
        .no_overflow = {},
        .in_bounds = {},
        .reserved_pushes = {},
    });
  }
  return facts;
}

std::vector<std::uint8_t> FindClosedRangeRoots(const ir::Program& program) {
  std::vector<std::uint8_t> roots(program.functions.size());
  for (std::size_t index = 0; index < roots.size(); ++index) {
    const auto& function = program.functions[index];
    roots[index] =
        static_cast<std::uint8_t>(function.is_exported || function.parameter_count == 0 ||
                                  !ir::CanAnalyzeSynchronousFunction(function));
  }
  const auto expose = [&](std::uint32_t target) { roots[target] = 1; };
  // callable、接口、构造器及属性入口可绕开直接调用边，因此参数必须保留完整类型域。
  for (const auto& type : program.classes) {
    ir::ForEachClassFunction(
        type, [&](std::uint32_t target, ir::ClassFunctionKind /*kind*/) { expose(target); });
  }
  for (const auto& member : program.members) {
    ir::ForEachPropertyFunction(member, expose);
  }
  for (const auto& plan : program.json_decode_plans) {
    if (plan.constructor != kInvalidFunctionTarget) {
      expose(plan.constructor);
    }
  }
  return roots;
}

std::vector<std::uint8_t> ProveArrayAccessBounds(
    const ir::Function& function,
    const IntegerRangeFacts& integers,
    std::span<const std::optional<std::uint32_t>> lengths) {
  std::vector<std::uint8_t> in_bounds(function.instructions.size());
  for (std::size_t position = 0; position < function.instructions.size(); ++position) {
    const auto& instruction = function.instructions[position];
    if (instruction.opcode != Opcode::GetIndex && instruction.opcode != Opcode::SetIndex) {
      continue;
    }
    const auto reg = instruction.opcode == Opcode::GetIndex ? instruction.lhs : instruction.dst;
    const auto length = lengths[reg];
    const auto& range = integers.indices[position];
    if (length && !range.isEmptySet() && range.getUnsignedMax().ult(*length)) {
      in_bounds[position] = 1;
    }
  }
  return in_bounds;
}

}  // namespace

ProgramArrayAccessFacts AnalyzeArrayAccess(const ir::Program& program, ArrayAnalysisMode mode) {
  const auto track_storage = mode != ArrayAnalysisMode::Elements;
  if (!track_storage) {
    const auto has_integer_array =
        std::ranges::any_of(program.functions, [&](const auto& function) {
          return std::ranges::any_of(function.instructions, [&](const auto& instruction) {
            return HasLocalArrayDefinition(instruction, program);
          });
        });
    if (!has_integer_array) {
      return {.generic = std::vector<ArrayAccessFacts>(program.functions.size()), .closed = {}};
    }
  }
  std::vector<std::vector<ir::RegisterAccess>> accesses(program.functions.size());
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    accesses[index] = ir::AnalyzeRegisterAccesses(program.functions[index], program.functions);
  }
  auto generic = PackArrayAccessFacts(
      ArrayDomainAnalysis(program, ArrayCallDomain::Generic, accesses, mode).Run());
  ProgramArrayAccessFacts result{.generic = std::move(generic), .closed = {}};
  if (track_storage) {
    for (std::size_t index = 0; index < program.functions.size(); ++index) {
      result.generic[index].reserved_pushes =
          FindReservedArrayPushes(program.functions[index], accesses[index]);
    }
  }
  if (mode != ArrayAnalysisMode::Ranges) {
    return result;
  }
  auto roots = FindClosedRangeRoots(program);
  IntegerRangeAnalysis range_analysis(program, roots, accesses);
  auto integers = range_analysis.Run();
  auto direct =
      ArrayDomainAnalysis(program, ArrayCallDomain::DirectCalls, accesses, mode, roots, integers)
          .Run();
  integers = range_analysis.Run(direct.bits);
  auto& closed = result.closed;
  closed.roots = std::move(roots);
  closed.reachable.reserve(program.functions.size());
  closed.parameters.reserve(program.functions.size());
  const auto lengths = std::move(direct.storage.minimum_lengths);
  closed.functions = PackArrayAccessFacts(std::move(direct));
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    auto& access = closed.functions[index];
    auto& integer = integers[index];
    access.in_bounds = ProveArrayAccessBounds(program.functions[index], integer, lengths[index]);
    access.no_overflow = std::move(integer.no_overflow);
    closed.reachable.push_back(static_cast<std::uint8_t>(integer.reachable));
    closed.parameters.push_back(std::move(integer.parameters));
    const auto& generic_bits = result.generic[index].element_bits;
    for (std::size_t reg = 0; reg < generic_bits.size(); ++reg) {
      access.element_bits[reg] &= generic_bits[reg];
    }
  }
  return result;
}

}  // namespace vela::compiler::native
