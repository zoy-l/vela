#include <cstddef>
#include <cstdint>
#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"

namespace vela::ir {

auto Lowerer::RegisterArrayType(const frontend::TypeDescriptor& type) -> std::uint32_t {
  if (type.nullable) {
    return RegisterArrayType(model_.types.at(type.name.substr(0, type.name.size() - 1)));
  }
  const auto found = array_type_ids_.find(type.name);
  if (found != array_type_ids_.end()) {
    return found->second;
  }
  const auto& element = model_.array_elements.at(type.name);
  if (element.kind == frontend::TypeDescriptor::Kind::Set ||
      element.kind == frontend::TypeDescriptor::Kind::Map) {
    static_cast<void>(RegisterCollectionType(element));
  }
  if (element.kind == frontend::TypeDescriptor::Kind::Expected) {
    static_cast<void>(RegisterExpectedType(element));
  }
  if (element.kind == frontend::TypeDescriptor::Kind::Array) {
    static_cast<void>(RegisterArrayType(element));
  }
  const auto index = static_cast<std::uint32_t>(program_.array_types.size());
  program_.array_types.push_back(type);
  array_type_ids_.emplace(type.name, index);
  return index;
}

auto Lowerer::LowerArrayLiteral(const frontend::Expression& expression) -> LoweredValue {
  const auto& type = model_.expression_types.at(&expression);
  const RegisterId array = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::NewArray,
      .dst = array,
      .immediate = static_cast<std::int64_t>(expression.elements.size()),
      .index = RegisterArrayType(type),
      .location = expression.location,
  });
  for (std::size_t index = 0; index < expression.elements.size(); ++index) {
    const LoweredValue element = LowerExpression(*expression.elements.at(index));
    const RegisterId index_reg = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = index_reg,
        .immediate = static_cast<std::int64_t>(index),
        .location = expression.location,
    });
    Emit({
        .opcode = Opcode::SetIndex,
        .dst = array,
        .lhs = index_reg,
        .rhs = element.reg,
        .location = expression.location,
    });
  }
  return {.reg = array, .type = type};
}

auto Lowerer::LowerIndex(const frontend::Expression& expression) -> LoweredValue {
  const LoweredValue array = SnapshotBefore(LowerExpression(*expression.object), *expression.index);
  const LoweredValue index = LowerExpression(*expression.index);
  const RegisterId result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::GetIndex,
      .dst = result,
      .lhs = array.reg,
      .rhs = index.reg,
      .location = expression.location,
  });
  return {
      .reg = result,
      .type = model_.array_elements.at(array.type.name),
  };
}

}  // namespace vela::ir
