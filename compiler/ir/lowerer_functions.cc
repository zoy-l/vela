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

auto Lowerer::ReadLocal(const Local& local, frontend::SourceLocation location) -> LoweredValue {
  if (!local.cell) {
    return {.reg = local.reg, .type = local.type};
  }
  const auto value = AllocateTemporary(location);
  Emit({.opcode = Opcode::GetField, .dst = value, .lhs = local.reg, .location = location});
  return {.reg = value, .type = local.type};
}

auto Lowerer::CallableClass(std::uint32_t function_id,
                            CallableMode mode,
                            const std::vector<frontend::Capture>& captures) -> ClassId {
  const auto key = (static_cast<std::uint64_t>(mode) << 32U) | function_id;
  const auto existing = callable_classes_.find(key);
  if (existing != callable_classes_.end()) {
    return existing->second;
  }
  const auto layout_id = static_cast<ClassId>(program_.classes.size());
  Class layout;
  layout.name = "$callable" + std::to_string(key);
  layout.callable = CallableTarget{.function = function_id, .mode = mode};
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

auto Lowerer::LowerFunctionValue(const frontend::Expression& expression) -> LoweredValue {
  const bool lambda = expression.kind == frontend::Expression::Kind::Lambda;
  const auto interface = model_.interface_members.find(&expression);
  const auto static_member = model_.static_members.find(&expression);
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
    receiver = LowerExpression(*expression.object);
    target =
        interface == model_.interface_members.end()
            ? function_ids_.at(receiver->type.name + "." + expression.text)
            : interface_method_ids_.at(interface->second.owner + "." + interface->second.member);
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
  const auto layout = CallableClass(target, mode, captures);
  const auto object = AllocateTemporary(expression.location);
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
  return {.reg = object, .type = model_.expression_types.at(&expression)};
}

auto Lowerer::LowerIndirectCall(const frontend::Expression& expression) -> LoweredValue {
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

auto Lowerer::LowerInterfaceCall(const frontend::Expression& expression) -> LoweredValue {
  const auto& member = model_.interface_members.at(&expression);
  const auto receiver = LowerExpression(*expression.callee->object);
  const auto& signature = model_.interfaces.at(member.owner).methods.at(member.member);
  return LowerInvocation(expression, signature,
                         interface_method_ids_.at(member.owner + "." + member.member), &receiver,
                         Opcode::CallInterface);
}

}  // namespace vela::ir
