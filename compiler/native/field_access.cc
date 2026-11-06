#include "compiler/native/field_access.h"
#include <absl/container/flat_hash_map.h>

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

class FieldKindKnowledge final {
 public:
  explicit FieldKindKnowledge(std::size_t registers) : definitions_(registers) {}

  void Clear() {
    fields_.clear();
    writes_.clear();
  }

  void Define(ir::RegisterId reg) { ++definitions_.at(reg); }

  void Read(const ir::Instruction& instruction, const ir::Function& function) {
    const auto kind = ScalarKind(function.value_types.at(instruction.dst));
    if (kind) {
      // Typed IR 的非空标量字段读取只能观察已初始化、类型匹配的字段。
      Remember(instruction.lhs, instruction.index, *kind);
    }
  }

  FieldKindWrite Write(const ir::Instruction& instruction, const ir::Function& function) {
    const auto kind = ScalarKind(function.value_types.at(instruction.lhs));
    const auto object = instruction.dst;
    const auto slot = instruction.index;
    auto& epoch = writes_[slot];
    const auto previous = fields_.find({object, slot});
    const auto redundant = kind && previous != fields_.end() && previous->second.kind == *kind &&
                           previous->second.definition == definitions_.at(object) &&
                           previous->second.write == epoch;
    // 不同寄存器可能指向同一对象；相同物理槽的任何写入都会使旧事实失效。
    ++epoch;
    if (kind) {
      Remember(object, slot, *kind);
    }
    return redundant ? FieldKindWrite::Redundant : FieldKindWrite::Required;
  }

 private:
  struct Fact final {
    Kind kind;
    std::size_t definition;
    std::size_t write;
  };

  void Remember(ir::RegisterId object, ir::FieldId slot, Kind kind) {
    fields_.insert_or_assign(
        std::pair{object, slot},
        Fact{.kind = kind, .definition = definitions_.at(object), .write = writes_[slot]});
  }

  std::vector<std::size_t> definitions_;
  absl::flat_hash_map<ir::FieldId, std::size_t> writes_;
  absl::flat_hash_map<std::pair<ir::RegisterId, ir::FieldId>, Fact> fields_;
};

}  // namespace

absl::flat_hash_map<DeclarationId, FieldTarget> FindFieldTargets(const ir::Program& program) {
  absl::flat_hash_map<DeclarationId, FieldTarget> fields;
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    const auto& type = program.classes.at(index);
    for (std::size_t slot = 0; slot < type.fields.size(); ++slot) {
      const auto declaration = type.fields.at(slot).declaration;
      if (declaration != DeclarationId{}) {
        fields.try_emplace(declaration, FieldTarget{
                                            .owner = static_cast<ir::ClassId>(index),
                                            .slot = static_cast<ir::FieldId>(slot),
                                        });
      }
    }
  }
  return fields;
}

std::vector<FieldKindWrite> FindFieldKindWrites(const ir::Function& function,
                                                const ir::Program& program,
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
      const auto effects = ir::AnalyzeEffects(instruction);
      if (safepoints[index] != 0 || effects.MayCollectOnSuccess() ||
          (effects.writes_state && opcode != Opcode::SetField)) {
        known.Clear();
      }
      if (opcode == Opcode::GetField) {
        known.Read(instruction, function);
      } else if (opcode == Opcode::SetField) {
        writes.at(index) = known.Write(instruction, function);
      }
      if (ir::AnalyzeRegisters(instruction, program.functions).defines_value) {
        known.Define(instruction.dst);
      }
    }
  }
  return writes;
}

}  // namespace vela::compiler::native
