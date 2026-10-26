#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"

namespace vela::ir {

Lowerer::LoweredValue Lowerer::LowerArrayMutation(const frontend::Expression& expression) {
  auto builtin = model_.builtin_calls.at(&expression);
  const auto location = expression.location;
  const auto base = next_register_;
  const bool splice = builtin == Builtin::ArraySplice;
  const auto arity = BuiltinArity(builtin);
  for (std::size_t index = 0; index < arity; ++index) {
    static_cast<void>(AllocateRegister(location));
  }
  const auto receiver = LowerExpression(*expression.callee->object);
  Emit({.opcode = Opcode::Move, .dst = base, .lhs = receiver.reg, .location = location});
  if (!splice && expression.arguments.empty()) {
    const auto result = EmitTemporary({
        .opcode = Opcode::BuiltinCall,
        .lhs = base,
        .index = static_cast<std::uint32_t>(Builtin::ArrayLength),
        .location = location,
    });
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }
  if (splice) {
    Emit({.opcode = Opcode::LoadInt, .dst = base + 1, .location = location});
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = base + 2,
        .immediate = expression.arguments.empty() ? 0 : std::numeric_limits<std::int64_t>::max(),
        .location = location,
    });
  }
  const auto& order = model_.argument_order.at(&expression);
  std::vector<RegisterId> items;
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    const auto value = LowerExpression(*expression.arguments.at(index).value);
    if (splice && order.at(index) < 2) {
      Emit({
          .opcode = Opcode::Move,
          .dst = base + 1 + order.at(index),
          .lhs = value.reg,
          .location = location,
      });
    } else {
      items.push_back(
          EmitTemporary({.opcode = Opcode::Move, .lhs = value.reg, .location = location}));
    }
  }
  const auto packed = base + (splice ? 3 : 1);
  Emit({
      .opcode = Opcode::NewArray,
      .dst = packed,
      .immediate = static_cast<std::int64_t>(items.size()),
      .index = RegisterArrayType(receiver.type),
      .location = location,
  });
  for (std::size_t index = 0; index < items.size(); ++index) {
    const auto position = EmitTemporary({
        .opcode = Opcode::LoadInt,
        .immediate = static_cast<std::int64_t>(index),
        .location = location,
    });
    Emit({
        .opcode = Opcode::SetIndex,
        .dst = packed,
        .lhs = position,
        .rhs = items.at(index),
        .location = location,
    });
  }
  if (builtin == Builtin::ArrayPush) {
    builtin = Builtin::ArrayPushMany;
  }
  const auto result = EmitTemporary({
      .opcode = Opcode::BuiltinCall,
      .lhs = base,
      .index = static_cast<std::uint32_t>(builtin),
      .location = location,
  });
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

std::uint32_t Lowerer::RegisterArrayType(const frontend::TypeDescriptor& type) {
  if (type.nullable) {
    return RegisterArrayType(model_.types.at(type.name.substr(0, type.name.size() - 1)));
  }
  const auto found = array_type_ids_.find(type.name);
  if (found != array_type_ids_.end()) {
    return found->second;
  }
  const auto& element = model_.array_elements.at(type.name);
  if (element.kind == frontend::TypeDescriptor::Kind::Enum) {
    static_cast<void>(RegisterEnumType(element));
  }
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

Lowerer::LoweredValue Lowerer::LowerArrayLiteral(const frontend::Expression& expression) {
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

Lowerer::LoweredValue Lowerer::LowerIndex(const frontend::Expression& expression) {
  const LoweredValue array = LowerExpression(*expression.object);
  const RegisterId result = AllocateTemporary(expression.location);
  if (expression.null_safe) {
    const auto null_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfNull, .lhs = array.reg, .location = expression.location});
    const LoweredValue index = LowerExpression(*expression.index);
    EmitIndexRead(result, array, index.reg, expression.location);
    const auto end_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = expression.location});
    current_function_->instructions.at(null_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    Emit({.opcode = Opcode::LoadNull, .dst = result, .location = expression.location});
    current_function_->instructions.at(end_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }
  const LoweredValue stable_array = SnapshotBefore(array, *expression.index);
  const LoweredValue index = LowerExpression(*expression.index);
  EmitIndexRead(result, stable_array, index.reg, expression.location);
  return {
      .reg = result,
      .type = model_.expression_types.at(&expression),
  };
}

}  // namespace vela::ir
