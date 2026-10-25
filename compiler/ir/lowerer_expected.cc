#include "compiler/ir/lowerer.h"

#include <utility>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

auto Lowerer::RegisterExpectedType(frontend::TypeDescriptor type) -> ClassId {
  if (type.nullable) {
    type = model_.types.at(type.name.substr(0, type.name.size() - 1));
  }
  const auto found = class_ids_.find(type.name);
  if (found != class_ids_.end()) {
    return found->second;
  }
  Class layout;
  layout.name = type.name;
  layout.kind = Class::Kind::Expected;
  layout.field_types = model_.generic_arguments.at(type.name);
  const auto layout_id = static_cast<ClassId>(program_.classes.size());
  program_.classes.push_back(std::move(layout));
  class_ids_.emplace(type.name, layout_id);
  return layout_id;
}

auto Lowerer::LowerExpected(const frontend::Expression& expression) -> LoweredValue {
  const auto operation = model_.expected_calls.at(&expression);
  const auto result = AllocateTemporary(expression.location);
  if (operation == frontend::ExpectedOperation::Ok ||
      operation == frontend::ExpectedOperation::Err) {
    const auto layout = RegisterExpectedType(model_.expression_types.at(&expression));
    RegisterId payload = 0;
    if (expression.arguments.empty()) {
      payload = AllocateTemporary(expression.location);
      Emit({.opcode = Opcode::LoadNull, .dst = payload, .location = expression.location});
    } else {
      payload = LowerExpression(*expression.arguments.front().value).reg;
    }
    Emit({
        .opcode = operation == frontend::ExpectedOperation::Ok ? Opcode::NewOk : Opcode::NewErr,
        .dst = result,
        .lhs = payload,
        .index = layout,
        .location = expression.location,
    });
  } else {
    const auto receiver = LowerExpression(*expression.callee->object);
    Emit({
        .opcode = operation == frontend::ExpectedOperation::HasValue ? Opcode::ExpectedHasValue
                                                                     : Opcode::ExpectedValue,
        .dst = result,
        .lhs = receiver.reg,
        .index = operation == frontend::ExpectedOperation::Error ? 1U : 0U,
        .location = expression.location,
    });
  }
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

}  // namespace vela::ir
