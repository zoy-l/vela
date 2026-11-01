#include "compiler/native/field_access.h"
#include <absl/container/flat_hash_map.h>

#include <cstddef>
#include "base/declaration.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

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

}  // namespace vela::compiler::native
