#include "compiler/ir/lowerer.h"

#include <cstdint>
#include <string>

#include "base/builtin.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

std::uint32_t Lowerer::RegisterCollectionType(frontend::TypeDescriptor type) {
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
    const auto array_name = argument.kind == frontend::TypeDescriptor::Kind::Function
                                ? "(" + std::string(argument.name) + ")[]"
                                : std::string(argument.name) + "[]";
    static_cast<void>(RegisterArrayType(model_.types.at(array_name)));
  }
  return index;
}

Lowerer::LoweredValue Lowerer::LowerCollectionConstruction(const frontend::Expression& expression) {
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

Lowerer::LoweredValue Lowerer::LowerMapLiteral(const frontend::Expression& expression) {
  const auto map = LowerCollectionConstruction(expression);
  for (const auto& entry : expression.entries) {
    const auto location = entry.key->location;
    const auto base = AllocateRegisterWindow(BuiltinArity(Builtin::MapSet), location);
    Emit({.opcode = Opcode::Move, .dst = base, .lhs = map.reg, .location = location});
    const auto key = LowerExpression(*entry.key);
    // 在值表达式执行前保存已转换的键，防止赋值、调用或 await 改变键的来源。
    Emit({.opcode = Opcode::Move, .dst = base + 1, .lhs = key.reg, .location = location});
    const auto value = LowerExpression(*entry.value);
    Emit({
        .opcode = Opcode::Move,
        .dst = base + 2,
        .lhs = value.reg,
        .location = entry.value->location,
    });
    const auto result = AllocateTemporary(location);
    RecordValueType(
        result, {.kind = frontend::TypeDescriptor::Kind::Void, .name = frontend::kVoidTypeName});
    EmitBuiltinCall(Builtin::MapSet, result, base, location);
  }
  return map;
}

}  // namespace vela::ir
