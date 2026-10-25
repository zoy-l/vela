#include "compiler/ir/lowerer.h"

#include <cstdint>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

auto Lowerer::RegisterCollectionType(frontend::TypeDescriptor type) -> std::uint32_t {
  if (type.nullable) {
    type = model_.types.at(type.name.substr(0, type.name.size() - 1));
  }
  if (const auto found = collection_type_ids_.find(type.name);
      found != collection_type_ids_.end()) {
    return found->second;
  }
  const auto index = static_cast<std::uint32_t>(program_.collection_types.size());
  collection_type_ids_.emplace(type.name, index);
  program_.collection_types.push_back(type);
  for (const auto& argument : model_.generic_arguments.at(type.name)) {
    const auto array_name =
        (argument.kind == frontend::TypeDescriptor::Kind::Function ? "(" + argument.name + ")"
                                                                   : argument.name) +
        "[]";
    static_cast<void>(RegisterArrayType(model_.types.at(array_name)));
  }
  return index;
}

auto Lowerer::LowerCollectionConstruction(const frontend::Expression& expression) -> LoweredValue {
  const auto& type = model_.expression_types.at(&expression);
  const auto layout = RegisterCollectionType(type);
  const auto result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::NewCollection,
      .dst = result,
      .index = layout,
      .location = expression.location,
  });
  return {.reg = result, .type = type};
}

}  // namespace vela::ir
