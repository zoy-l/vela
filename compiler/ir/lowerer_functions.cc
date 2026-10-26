#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

Lowerer::LoweredValue Lowerer::ReadLocal(const Local& local, frontend::SourceLocation location) {
  if (!local.cell) {
    return {.reg = local.reg, .type = local.type};
  }
  const auto value = AllocateTemporary(location);
  Emit({.opcode = Opcode::GetField, .dst = value, .lhs = local.reg, .location = location});
  return {.reg = value, .type = local.type};
}

ClassId Lowerer::CallableClass(std::uint32_t function_id,
                               CallableMode mode,
                               const std::vector<frontend::Capture>& captures,
                               const frontend::TypeDescriptor& type) {
  const auto key = (static_cast<std::uint64_t>(mode) << 32U) | function_id;
  const auto existing = callable_classes_.find(key);
  if (existing != callable_classes_.end()) {
    return existing->second;
  }
  const auto layout_id = static_cast<ClassId>(program_.classes.size());
  Class layout;
  layout.name = "$callable" + std::to_string(key);
  layout.callable = CallableTarget{.function = function_id, .mode = mode};
  layout.callable_type = type.nullable ? type.name.substr(1, type.name.size() - 3) : type.name;
  for (const auto& capture : captures) {
    layout.field_order.push_back(capture.name);
    layout.field_types.push_back(
        capture.kind == frontend::BindingKind::Mutable
            ? frontend::TypeDescriptor{.kind = frontend::TypeDescriptor::Kind::Class,
                                       .name = "$Cell",}
            : capture.type);
  }
  program_.classes.push_back(std::move(layout));
  callable_classes_.emplace(key, layout_id);
  return layout_id;
}

Lowerer::LoweredValue Lowerer::LowerFunctionValue(const frontend::Expression& expression) {
  const bool lambda = expression.kind == frontend::Expression::Kind::Lambda;
  const auto interface = model_.interface_members.find(&expression);
  const auto static_member = model_.static_members.find(&expression);
  const auto builtin = model_.builtin_method_values.find(&expression);
  const auto super = model_.super_members.find(&expression);
  const bool method = expression.kind == frontend::Expression::Kind::Member &&
                      static_member == model_.static_members.end();
  std::vector<frontend::Capture> captures;
  std::uint32_t target = 0;
  std::optional<LoweredValue> receiver;
  if (lambda) {
    const auto index = model_.lambda_indices.at(&expression);
    target = lambda_ids_.at(index);
    captures = model_.lambdas.at(index).captures;
  } else if (method) {
    if (super != model_.super_members.end()) {
      const auto* self = FindLocal("this");
      receiver = ReadLocal(*self, expression.location);
      target = function_ids_.at(FindMethodOwner(super->second.owner, expression.text) + "." +
                                expression.text);
    } else {
      receiver = LowerExpression(*expression.object);
      const auto name = receiver->type.nullable
                            ? receiver->type.name.substr(0, receiver->type.name.size() - 1)
                            : receiver->type.name;
      if (builtin != model_.builtin_method_values.end()) {
        target = function_ids_.at(model_.generated_functions.at(builtin->second).declaration->name);
      } else if (interface != model_.interface_members.end()) {
        target = interface_method_ids_.at(interface->second.owner + "." + interface->second.member);
      } else {
        target = function_ids_.at(FindMethodOwner(name, expression.text) + "." + expression.text);
      }
    }
    captures.push_back({
        .name = "this",
        .type = receiver->type,
        .binding = 0,
        .kind = frontend::BindingKind::Constant,
    });
  } else if (static_member != model_.static_members.end()) {
    target = function_ids_.at(static_member->second.owner + "." + static_member->second.member);
  } else {
    target = function_ids_.at(expression.text);
  }
  auto mode = CallableMode::Plain;
  if (lambda) {
    mode = CallableMode::Closure;
  } else if (interface != model_.interface_members.end()) {
    mode = CallableMode::InterfaceMethod;
  } else if (method) {
    mode = CallableMode::BoundMethod;
  }
  const auto layout =
      CallableClass(target, mode, captures, model_.expression_types.at(&expression));
  const auto object = AllocateTemporary(expression.location);
  std::optional<std::size_t> null_jump;
  if (receiver && expression.null_safe) {
    null_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfNull, .lhs = receiver->reg, .location = expression.location});
  }
  Emit({
      .opcode = Opcode::NewInstance,
      .dst = object,
      .index = layout,
      .location = expression.location,
  });
  for (std::size_t index = 0; index < captures.size(); ++index) {
    const auto reg = receiver ? receiver->reg : binding_registers_.at(captures.at(index).binding);
    Emit({
        .opcode = Opcode::SetField,
        .dst = object,
        .lhs = reg,
        .index = static_cast<FieldId>(index),
        .location = expression.location,
    });
  }
  if (null_jump) {
    const auto end_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = expression.location});
    current_function_->instructions.at(*null_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    Emit({.opcode = Opcode::LoadNull, .dst = object, .location = expression.location});
    current_function_->instructions.at(end_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
  return {.reg = object, .type = model_.expression_types.at(&expression)};
}

Lowerer::LoweredValue Lowerer::LowerIndirectCall(const frontend::Expression& expression) {
  const auto callee = LowerExpression(*expression.callee);
  // callee 也可能是可重写的局部变量，必须在求值实参前保存。
  const auto callable = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::Move,
      .dst = callable,
      .lhs = callee.reg,
      .location = expression.location,
  });
  const auto count = expression.arguments.size();
  const auto base = next_register_;
  for (std::size_t index = 0; index < count; ++index) {
    static_cast<void>(AllocateRegister(expression.location));
  }
  for (std::size_t index = 0; index < count; ++index) {
    const auto& argument = expression.arguments.at(index);
    const auto value = LowerExpression(*argument.value);
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + index),
        .lhs = value.reg,
        .location = argument.location,
    });
  }
  const auto result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::CallIndirect,
      .dst = result,
      .lhs = base,
      .rhs = callable,
      .index = static_cast<std::uint32_t>(count),
      .location = expression.location,
  });
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

Lowerer::LoweredValue Lowerer::LowerInterfaceCall(const frontend::Expression& expression) {
  const auto& member = model_.interface_members.at(&expression);
  const auto receiver = LowerExpression(*expression.callee->object);
  const auto& signature = model_.interfaces.at(member.owner).methods.at(member.member);
  return LowerInvocation(expression, signature,
                         interface_method_ids_.at(member.owner + "." + member.member), &receiver,
                         Opcode::CallInterface);
}

}  // namespace vela::ir
