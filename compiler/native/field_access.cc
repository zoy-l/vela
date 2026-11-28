#include "compiler/native/field_access.h"
#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/layout_description.h"
#include "compiler/native/native_roots.h"

namespace vela::compiler::native {
namespace {

using Kind = frontend::TypeDescriptor::Kind;

std::optional<Kind> ScalarKind(const frontend::TypeDescriptor& type) {
  if (!type.nullable && (type.kind == Kind::Bool || type.kind == Kind::Int ||
                         type.kind == Kind::Enum || type.kind == Kind::Double)) {
    return type.kind;
  }
  return std::nullopt;
}

struct FixedFieldKinds final {
  Kind kind;
  const frontend::StructLayout* layout = nullptr;

  bool operator==(const FixedFieldKinds&) const = default;
};

bool HasFieldKindInvalidation(Opcode opcode,
                              ir::InstructionEffects effects,
                              std::uint8_t safepoint,
                              const std::optional<FixedFieldKinds>& kinds) {
  // 封闭类字段直接写入内联槽。通用 IR 为可能的装箱预留的安全点不会执行。
  const auto inline_store = opcode == Opcode::SetField && kinds && kinds->layout != nullptr;
  const auto has_safepoint =
      safepoint != 0 && (!inline_store || safepoint != kNativeSafepointBeforeRuntime);
  return has_safepoint || (!inline_store && effects.MayCollectOnSuccess()) ||
         (effects.writes_state && opcode != Opcode::SetField);
}

class FieldKindKnowledge final {
 public:
  explicit FieldKindKnowledge(std::size_t registers) : definitions_(registers) {}

  void Clear() {
    fields_.clear();
    writes_.clear();
  }

  void Define(ir::RegisterId reg) { ++definitions_.at(reg); }

  static std::optional<FixedFieldKinds> Describe(const ir::Instruction& instruction,
                                                 const ir::Function& function,
                                                 const FieldTargets& targets) {
    const auto reading = instruction.opcode == Opcode::GetField;
    const auto& type = function.value_types.at(reading ? instruction.dst : instruction.lhs);
    if (const auto scalar = ScalarKind(type)) {
      return FixedFieldKinds{.kind = *scalar};
    }
    if (type.value_layout == nullptr || instruction.field_declaration) {
      return std::nullopt;
    }
    const auto& receiver = function.value_types.at(reading ? instruction.lhs : instruction.dst);
    if (receiver.kind != Kind::Class) {
      return std::nullopt;
    }
    const auto storage = targets.Describe(function, instruction);
    if (storage.owner == nullptr || !storage.HasInlineValue() || storage.type->nullable) {
      return std::nullopt;
    }
    const auto* layout = storage.type->value_layout.get();
    if (!std::ranges::all_of(layout->slots,
                             [](const auto& leaf) { return ScalarKind(leaf).has_value(); })) {
      return std::nullopt;
    }
    return FixedFieldKinds{.kind = storage.type->kind, .layout = layout};
  }

  void Read(const ir::Instruction& instruction, std::optional<FixedFieldKinds> kinds) {
    if (kinds) {
      // Typed IR 的固定类型字段读取只能观察已初始化、类型匹配的字段存储。
      Remember(instruction.lhs, instruction.index, *kinds);
    }
  }

  FieldKindWrite Write(const ir::Instruction& instruction, std::optional<FixedFieldKinds> kinds) {
    const auto object = instruction.dst;
    const auto slot = instruction.index;
    auto& epoch = writes_[slot];
    const auto previous = fields_.find({object, slot});
    const auto redundant = kinds && previous != fields_.end() && previous->second.kinds == *kinds &&
                           previous->second.definition == definitions_.at(object) &&
                           previous->second.write == epoch;
    // 不同寄存器可能指向同一对象；相同物理槽的任何写入都会使旧事实失效。
    ++epoch;
    if (kinds) {
      Remember(object, slot, *kinds);
    }
    return redundant ? FieldKindWrite::Redundant : FieldKindWrite::Required;
  }

 private:
  struct Fact final {
    FixedFieldKinds kinds;
    std::size_t definition;
    std::size_t write;
  };

  void Remember(ir::RegisterId object, ir::FieldId slot, FixedFieldKinds kinds) {
    fields_.insert_or_assign(
        std::pair{object, slot},
        Fact{.kinds = kinds, .definition = definitions_.at(object), .write = writes_[slot]});
  }

  std::vector<std::size_t> definitions_;
  absl::flat_hash_map<ir::FieldId, std::size_t> writes_;
  absl::flat_hash_map<std::pair<ir::RegisterId, ir::FieldId>, Fact> fields_;
};

}  // namespace

const ir::Class* FieldTargets::FindClass(const frontend::TypeDescriptor& type) const {
  const auto owner = classes.find(frontend::NonNullableTypeName(type));
  return owner == classes.end() ? nullptr : owner->second;
}

FieldStorage FieldTargets::Describe(const ir::Function& function,
                                    const ir::Instruction& instruction) const {
  const auto reading = instruction.opcode == Opcode::GetField;
  const auto* owner =
      FindClass(function.value_types.at(reading ? instruction.lhs : instruction.dst));
  const auto& type = owner != nullptr && owner->kind != ir::Class::Kind::CaptureCell
                         ? owner->field_types.at(instruction.index)
                         : function.value_types.at(reading ? instruction.dst : instruction.lhs);
  return {.type = &type, .owner = owner};
}

FieldTargets FindFieldTargets(const ir::Program& program) {
  FieldTargets fields;
  fields.classes.reserve(program.classes.size());
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    const auto& type = program.classes.at(index);
    fields.classes.try_emplace(type.name, &type);
    for (std::size_t slot = 0; slot < type.fields.size(); ++slot) {
      const auto declaration = type.fields.at(slot).declaration;
      if (declaration != DeclarationId{}) {
        fields.declarations.try_emplace(
            declaration,
            FieldTarget{
                .owner = static_cast<ir::ClassId>(index),
                .slot = ir::PhysicalFieldOffset(type, static_cast<std::uint32_t>(slot)),
            });
      }
    }
  }
  return fields;
}

std::vector<FieldKindWrite> FindFieldKindWrites(const ir::Function& function,
                                                const ir::Program& program,
                                                const FieldTargets& targets,
                                                std::span<const std::uint8_t> safepoints) {
  std::vector<FieldKindWrite> writes(function.instructions.size(), FieldKindWrite::Required);
  // 可重载恢复入口可从基本块中部继续；其初始化和迁移仍由字段就绪协议负责。
  if (program.native_mode == NativeProgramMode::Reloadable) {
    return writes;
  }
  const auto graph = ir::BuildFlowGraph(function);
  FieldKindKnowledge known(function.register_count);
  for (const auto& block : graph.blocks) {
    known.Clear();
    for (auto index = block.begin; index < block.end; ++index) {
      const auto& instruction = function.instructions.at(index);
      const auto opcode = instruction.opcode;
      const auto access = ir::AnalyzeRegisters(instruction, program.functions);
      const auto effects = ir::AnalyzeEffects(function, instruction, access);
      const auto heap_field =
          !instruction.inline_value && (opcode == Opcode::GetField || opcode == Opcode::SetField);
      const auto kinds =
          heap_field ? FieldKindKnowledge::Describe(instruction, function, targets) : std::nullopt;
      if (HasFieldKindInvalidation(opcode, effects, safepoints[index], kinds)) {
        known.Clear();
      }
      if (heap_field) {
        if (opcode == Opcode::GetField) {
          known.Read(instruction, kinds);
        } else {
          writes.at(index) = known.Write(instruction, kinds);
        }
      }
      if (access.defines_value) {
        known.Define(instruction.dst);
      }
    }
  }
  return writes;
}

}  // namespace vela::compiler::native
