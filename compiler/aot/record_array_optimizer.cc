#include "compiler/aot/record_array_optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/declaration.h"
#include "base/layout_description.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::aot {
namespace {

using Kind = frontend::TypeDescriptor::Kind;

struct ValueUse final {
  std::size_t instruction;
  ir::RegisterId reg;
};

struct FunctionFlow final {
  std::vector<ir::RegisterAccess> accesses;
  ir::RegisterFlow values;
  ir::FlowGraph graph;
  std::vector<std::vector<ValueUse>> uses;
  std::vector<std::size_t> borrow_barriers;
};

FunctionFlow AnalyzeFunction(const ir::Function& function,
                             std::span<const ir::Function> functions) {
  auto accesses = ir::AnalyzeRegisterAccesses(function, functions);
  auto values = ir::AnalyzeRegisterFlow(function, accesses);
  FunctionFlow flow{
      .accesses = std::move(accesses),
      .values = std::move(values),
      .graph = ir::BuildFlowGraph(function),
      .uses = std::vector<std::vector<ValueUse>>(function.instructions.size()),
      .borrow_barriers = {0},
  };
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions[index];
    ir::ForEachUse(instruction, flow.accesses[index], [&](ir::RegisterId reg) {
      if (const auto source = flow.values.OriginDefinition(reg)) {
        flow.uses[*source].push_back({.instruction = index, .reg = reg});
      }
    });
    const auto barrier =
        ir::IsBackJump(instruction, index) || instruction.opcode == Opcode::Return ||
        (instruction.opcode != Opcode::GetField && instruction.opcode != Opcode::SetField &&
         ir::AnalyzeEffects(instruction).MayCollectOnSuccess());
    flow.borrow_barriers.push_back(flow.borrow_barriers.back() + static_cast<std::size_t>(barrier));
  }
  return flow;
}

struct RecordArray final {
  std::uint32_t global;
  std::uint32_t initializer;
  std::size_t allocation;
  std::uint32_t array_type;
  std::uint32_t type;
};

bool IsRecordClass(const ir::Class& type, const ir::Program& program) {
  return type.kind == ir::Class::Kind::Instance && !type.callable && !type.field_types.empty() &&
         std::ranges::all_of(type.field_types,
                             [](const auto& field) {
                               return !field.nullable &&
                                      (field.kind == Kind::Int || field.kind == Kind::Double);
                             }) &&
         (type.constructor == kInvalidFunctionTarget ||
          ir::IsTrivialConstructor(program.functions[type.constructor]));
}

std::optional<RecordArray> FindRecordArray(const ir::Program& program, std::uint32_t global) {
  const auto initializer = program.global_initializers[global];
  const auto& function = program.functions[initializer];
  if (!std::ranges::any_of(function.instructions, [](const auto& instruction) {
        return instruction.opcode == Opcode::NewArray && instruction.immediate == 0;
      })) {
    return std::nullopt;
  }
  const auto accesses = ir::AnalyzeRegisterAccesses(function, program.functions);
  const auto flow = ir::AnalyzeRegisterFlow(function, accesses);
  std::optional<std::size_t> origin;
  for (const auto& instruction : function.instructions) {
    if (instruction.opcode == Opcode::Return) {
      const auto source = flow.OriginDefinition(instruction.dst);
      if (!source || (origin && origin != source)) {
        return std::nullopt;
      }
      origin = source;
    } else if (instruction.opcode != Opcode::NewArray && instruction.opcode != Opcode::Move &&
               instruction.opcode != Opcode::LoadNull) {
      return std::nullopt;
    }
  }
  if (!origin) {
    return std::nullopt;
  }
  const auto& allocation = function.instructions[*origin];
  if (allocation.opcode != Opcode::NewArray || allocation.immediate != 0) {
    return std::nullopt;
  }
  const auto& element =
      program.layouts.arrays[program.layouts.array_types[allocation.index]].element;
  if (element.kind != layout::Type::Kind::Instance || element.nullable ||
      !IsRecordClass(program.classes[element.index], program)) {
    return std::nullopt;
  }
  return RecordArray{
      .global = global,
      .initializer = initializer,
      .allocation = *origin,
      .array_type = allocation.index,
      .type = element.index,
  };
}

bool IsArrayOrigin(const ir::Function& function,
                   const ir::RegisterFlow& flow,
                   std::size_t function_index,
                   const RecordArray& array,
                   ir::RegisterId reg) {
  const auto source = flow.OriginDefinition(reg);
  if (!source) {
    return false;
  }
  const auto& instruction = function.instructions[*source];
  return (instruction.opcode == Opcode::GetGlobal && instruction.index == array.global) ||
         (function_index == array.initializer && *source == array.allocation);
}

bool HasFieldOnlyUses(const ir::Function& function,
                      const FunctionFlow& flow,
                      std::size_t origin,
                      const ir::Class& type) {
  auto last = origin;
  const auto block = flow.graph.instruction_blocks[origin];
  for (const auto& use : flow.uses[origin]) {
    const auto index = use.instruction;
    const auto reg = use.reg;
    const auto& instruction = function.instructions[index];
    if (index <= origin || flow.graph.instruction_blocks[index] != block) {
      return false;
    }
    last = std::max(last, index);
    const auto move = instruction.opcode == Opcode::Move && instruction.lhs == reg &&
                      flow.values.OriginDefinition(instruction.dst) == origin;
    const auto field = instruction.index < type.field_types.size() &&
                       ((instruction.opcode == Opcode::GetField && instruction.lhs == reg) ||
                        (instruction.opcode == Opcode::SetField && instruction.dst == reg));
    if (!move && !field) {
      return false;
    }
  }
  // 队列动作可在安全点重入并改变数组；元素借用不能跨回边、返回轮询或运行时调用。
  return flow.borrow_barriers[last + 1] == flow.borrow_barriers[origin + 1];
}

bool HasExclusiveInitializedUses(const ir::Program& program,
                                 const ir::Function& function,
                                 const FunctionFlow& flow,
                                 std::size_t source,
                                 const RecordArray& array,
                                 std::size_t push) {
  const auto& appended = function.instructions[push];
  const auto block = flow.graph.instruction_blocks[source];
  const auto& type = program.classes[array.type];
  std::vector<bool> initialized(type.field_types.size());
  for (const auto& use : flow.uses[source]) {
    const auto index = use.instruction;
    const auto reg = use.reg;
    const auto& instruction = function.instructions[index];
    if (index <= source || index > push || flow.graph.instruction_blocks[index] != block) {
      return false;
    }
    switch (instruction.opcode) {
      case Opcode::Move:
        if (flow.values.OriginDefinition(instruction.dst) != source) {
          return false;
        }
        break;
      case Opcode::SetField:
        if (instruction.dst != reg || instruction.index >= initialized.size()) {
          return false;
        }
        initialized[instruction.index] = true;
        break;
      case Opcode::GetField:
        if (instruction.index >= initialized.size() || !initialized[instruction.index]) {
          return false;
        }
        break;
      case Opcode::Call:
        if (!ir::IsTrivialConstructor(program.functions[instruction.index])) {
          return false;
        }
        break;
      default:
        if (index != push || reg != appended.lhs + 1) {
          return false;
        }
        break;
    }
  }
  return std::ranges::all_of(initialized, [](bool field) { return field; });
}

bool IsUniqueRecordInput(const ir::Program& program,
                         const ir::Function& function,
                         const FunctionFlow& flow,
                         std::size_t push,
                         const RecordArray& array) {
  const auto source = flow.values.OriginDefinition(function.instructions[push].lhs + 1);
  if (!source || *source >= push) {
    return false;
  }
  const auto& allocation = function.instructions[*source];
  return allocation.opcode == Opcode::NewInstance && allocation.index == array.type &&
         flow.graph.instruction_blocks[push] == flow.graph.instruction_blocks[*source] &&
         HasExclusiveInitializedUses(program, function, flow, *source, array, push);
}

bool IsArrayUse(const ir::Instruction& instruction, ir::RegisterId reg) {
  if (instruction.lhs != reg) {
    return false;
  }
  if (instruction.opcode == Opcode::GetIndex) {
    return true;
  }
  if (instruction.opcode != Opcode::BuiltinCall) {
    return false;
  }
  const auto builtin = static_cast<Builtin>(instruction.index);
  if (IsArrayMetadataRead(builtin)) {
    return true;
  }
  switch (builtin) {
    case Builtin::ArrayReserve:
    case Builtin::ArrayClear:
    case Builtin::ArrayPush:
      return true;
    default:
      return false;
  }
}

bool IsReadOnlyBorrow(const ir::Function& function,
                      const FunctionFlow& flow,
                      std::size_t function_index,
                      const RecordArray& array) {
  // 读取者不跨未知调用、惰性初始化或结构写入保存元素。GC 不移动数组数据。
  return std::ranges::all_of(function.instructions, [&](const auto& instruction) {
    if (instruction.opcode == Opcode::GetGlobal && instruction.index == array.global) {
      return true;
    }
    if (instruction.opcode == Opcode::GetField || instruction.opcode == Opcode::SetField) {
      return true;
    }
    if (instruction.opcode == Opcode::BuiltinCall &&
        IsArrayOrigin(function, flow.values, function_index, array, instruction.lhs)) {
      const auto builtin = static_cast<Builtin>(instruction.index);
      if (!IsArrayMetadataRead(builtin)) {
        return false;
      }
    }
    return instruction.opcode == Opcode::Return ||
           !ir::AnalyzeEffects(instruction).MayCollectOnSuccess();
  });
}

bool IsNonEscapingArrayUse(const ir::Program& program,
                           std::size_t function_index,
                           const FunctionFlow& flow,
                           const RecordArray& array,
                           const ValueUse& use) {
  const auto& function = program.functions[function_index];
  const auto& instruction = function.instructions[use.instruction];
  const auto reg = use.reg;
  if (!IsArrayOrigin(function, flow.values, function_index, array, reg)) {
    return true;
  }
  if (instruction.opcode == Opcode::Move) {
    return instruction.lhs == reg &&
           IsArrayOrigin(function, flow.values, function_index, array, instruction.dst);
  }
  if (instruction.opcode == Opcode::Return && function_index == array.initializer) {
    return instruction.dst == reg;
  }
  if (!IsArrayUse(instruction, reg)) {
    return false;
  }
  if (instruction.opcode == Opcode::GetIndex) {
    return flow.values.OriginDefinition(instruction.dst) == use.instruction &&
           HasFieldOnlyUses(function, flow, use.instruction, program.classes[array.type]);
  }
  return !ir::IsBuiltinCall(instruction, Builtin::ArrayPush) ||
         IsUniqueRecordInput(program, function, flow, use.instruction, array);
}

bool CanPackFunction(const ir::Program& program,
                     std::size_t function_index,
                     const FunctionFlow& flow,
                     const RecordArray& array) {
  const auto& function = program.functions[function_index];
  bool used = function_index == array.initializer;
  bool reads = false;
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions[index];
    if ((instruction.opcode == Opcode::SetGlobal && instruction.index == array.global) ||
        (instruction.opcode == Opcode::Call && instruction.index == array.initializer)) {
      return false;
    }
    if (instruction.opcode == Opcode::GetGlobal && instruction.index == array.global) {
      used = true;
      if (flow.values.OriginDefinition(instruction.dst) != index) {
        return false;
      }
    }
    bool valid = true;
    reads |= instruction.opcode == Opcode::GetIndex &&
             IsArrayOrigin(function, flow.values, function_index, array, instruction.lhs);
    ir::ForEachUse(instruction, flow.accesses[index], [&](ir::RegisterId reg) {
      valid &= IsNonEscapingArrayUse(program, function_index, flow, array,
                                     {.instruction = index, .reg = reg});
    });
    if (!valid) {
      return false;
    }
  }
  return !used || (ir::CanAnalyzeSynchronousFunction(function) &&
                   (!reads || IsReadOnlyBorrow(function, flow, function_index, array)));
}

template <class ElementPredicate>
void RewriteElementTypes(ir::Function& function,
                         const ir::RegisterFlow& flow,
                         const ElementPredicate& is_element) {
  for (ir::RegisterId reg = 0; reg < function.register_count; ++reg) {
    const auto origin = flow.OriginDefinition(reg);
    if (origin && is_element(function.instructions[*origin])) {
      function.value_types[reg] = {.kind = Kind::Int, .name = frontend::kIntTypeName};
    }
  }
}

void RewriteFunction(ir::Function& function,
                     const FunctionFlow& flow,
                     std::size_t function_index,
                     const RecordArray& array,
                     std::uint32_t array_type) {
  const auto original = function.instructions;
  const auto is_element = [&](const ir::Instruction& instruction) {
    return instruction.opcode == Opcode::GetIndex &&
           IsArrayOrigin(function, flow.values, function_index, array, instruction.lhs);
  };
  RewriteElementTypes(function, flow.values, is_element);
  for (std::size_t index = 0; index < original.size(); ++index) {
    auto& instruction = function.instructions[index];
    const auto& source = original[index];
    if (is_element(source)) {
      instruction.opcode = Opcode::CheckRecordIndex;
      instruction.array_type = array_type;
    } else if (ir::IsBuiltinCall(source, Builtin::ArrayPush) &&
               IsArrayOrigin(function, flow.values, function_index, array, source.lhs)) {
      instruction.opcode = Opcode::PushRecord;
      instruction.rhs = source.lhs + 1;
      instruction.array_type = array_type;
    } else if (source.opcode == Opcode::GetField || source.opcode == Opcode::SetField) {
      const auto reading = source.opcode == Opcode::GetField;
      const auto receiver = reading ? source.lhs : source.dst;
      const auto origin = flow.values.OriginDefinition(receiver);
      if (!origin || !is_element(original[*origin])) {
        continue;
      }
      const auto owner = original[*origin].lhs;
      const auto record_index = original[*origin].dst;
      instruction.opcode = reading ? Opcode::GetRecordField : Opcode::SetRecordField;
      instruction.array_type = array_type;
      instruction.field_declaration.reset();
      if (reading) {
        instruction.lhs = owner;
        instruction.rhs = record_index;
      } else {
        instruction.dst = owner;
        instruction.lhs = record_index;
        instruction.rhs = source.lhs;
      }
    }
  }
}

}  // namespace

void PackPrivateRecordArrays(ir::Program& program) {
  if (program.global_initializers.empty()) {
    return;
  }
  constexpr auto kUnexposed = static_cast<std::uint32_t>(GlobalFlag::Private) |
                              static_cast<std::uint32_t>(GlobalFlag::ModuleLocal);
  std::vector<std::size_t> initializer_counts(program.functions.size());
  for (const auto initializer : program.global_initializers) {
    ++initializer_counts[initializer];
  }
  absl::flat_hash_set<DeclarationId> reflected;
  for (const auto& member : program.reflection_members) {
    reflected.insert(member.declaration);
  }
  std::vector<RecordArray> candidates;
  for (std::uint32_t global = 0; global < program.global_initializers.size(); ++global) {
    if ((program.global_flags[global] & kUnexposed) != 0 &&
        initializer_counts[program.global_initializers[global]] == 1 &&
        !reflected.contains(program.global_declarations[global])) {
      if (const auto candidate = FindRecordArray(program, global)) {
        candidates.push_back(*candidate);
      }
    }
  }
  if (candidates.empty()) {
    return;
  }
  std::vector<FunctionFlow> flows;
  flows.reserve(program.functions.size());
  for (const auto& function : program.functions) {
    flows.push_back(AnalyzeFunction(function, program.functions));
  }
  for (const auto& array : candidates) {
    bool eligible = true;
    for (std::size_t function = 0; function < program.functions.size(); ++function) {
      eligible &= CanPackFunction(program, function, flows[function], array);
    }
    if (!eligible) {
      continue;
    }
    auto layout = program.layouts.arrays[program.layouts.array_types[array.array_type]];
    layout.name += "$records/" + std::to_string(program.global_declarations[array.global].value);
    layout.record_fields =
        static_cast<std::uint32_t>(program.classes[array.type].field_types.size());
    const auto physical = static_cast<std::uint32_t>(program.layouts.arrays.size());
    program.layouts.arrays.push_back(std::move(layout));
    const auto array_type = static_cast<std::uint32_t>(program.layouts.array_types.size());
    program.layouts.array_types.push_back(physical);
    program.array_types.push_back(program.array_types[array.array_type]);
    // 先改写使用，再替换初始化器布局；来源分析继续使用同一份规范指令位置。
    for (std::size_t function = 0; function < program.functions.size(); ++function) {
      RewriteFunction(program.functions[function], flows[function], function, array, array_type);
    }
    program.functions[array.initializer].instructions[array.allocation].index = array_type;
  }
}

}  // namespace vela::compiler::aot
