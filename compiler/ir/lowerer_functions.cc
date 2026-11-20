#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/lowerer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

Lowerer::LoweredValue Lowerer::ReadLocal(const Local& local, frontend::SourceLocation location) {
  if (!local.cell) {
    return {.reg = local.reg, .type = local.type};
  }
  const auto value = AllocateRegister(location);
  RecordValueType(value, local.type);
  Emit({.opcode = Opcode::GetField, .dst = value, .lhs = local.reg, .location = location});
  return {.reg = value, .type = local.type};
}

frontend::Capture Lowerer::MakeReceiverCapture(const frontend::TypeDescriptor& type) {
  return {
      .name = std::string(frontend::kReceiverName),
      .type = type,
      .binding = 0,
      .kind = frontend::BindingKind::Constant,
  };
}

ClassId Lowerer::CallableClass(std::uint32_t function_id,
                               CallableMode mode,
                               std::span<const frontend::Capture> captures,
                               const frontend::FunctionDescriptor* virtual_signature) {
  const auto key = (static_cast<std::uint64_t>(mode) << 32U) | function_id;
  const auto existing = callable_classes_.find(key);
  if (existing != callable_classes_.end()) {
    return existing->second;
  }
  Class layout;
  layout.name = "$callable" + std::to_string(key);
  layout.callable = CallableTarget{.function = function_id, .mode = mode};
  if (virtual_signature != nullptr) {
    layout.callable_type = DescribeCallSignature(*virtual_signature);
  }
  for (const auto& capture : captures) {
    layout.field_types.push_back(
        capture.kind == frontend::BindingKind::Mutable
            ? frontend::TypeDescriptor{.kind = frontend::TypeDescriptor::Kind::Class,
                                       .name = "$Cell",}
            : capture.type);
  }
  const auto layout_id = static_cast<ClassId>(program_.classes.size());
  program_.classes.push_back(std::move(layout));
  callable_classes_.emplace(key, layout_id);
  return layout_id;
}

Lowerer::FunctionValuePlan Lowerer::ResolveFunctionValue(const frontend::Expression& expression) {
  FunctionValuePlan plan;
  if (expression.kind == frontend::Expression::Kind::Lambda) {
    const auto index = model_.lambda_indices.at(&expression);
    plan.target = lambda_ids_.at(index);
    plan.captures = model_.lambdas.at(index).captures;
    plan.mode = CallableMode::Closure;
    return plan;
  }
  const auto static_member = model_.static_members.find(&expression);
  if (static_member != model_.static_members.end()) {
    const auto& member = static_member->second;
    plan.target =
        function_ids_.at(QualifiedMemberKey({.owner = member.owner, .name = member.member}));
    return plan;
  }
  if (expression.kind == frontend::Expression::Kind::Member) {
    return ResolveMethodValue(expression);
  }
  const auto application = model_.generic_applications.find(&expression);
  plan.target = function_ids_.at(
      application == model_.generic_applications.end() ? expression.text : application->second);
  return plan;
}

Lowerer::FunctionValuePlan Lowerer::ResolveMethodValue(const frontend::Expression& expression) {
  FunctionValuePlan plan;
  plan.mode = CallableMode::BoundMethod;
  const auto receiver = LowerMemberReceiver(expression);
  plan.receiver = receiver;
  const auto super = model_.super_members.find(&expression);
  const auto builtin = model_.builtin_method_values.find(&expression);
  const auto interface = model_.interface_members.find(&expression);
  if (super != model_.super_members.end()) {
    const auto& member = super->second;
    plan.target =
        function_ids_.at(QualifiedMemberKey({.owner = member.owner, .name = member.member}));
    const auto& parent = MemberClass(expression, receiver.type);
    plan.binding_receiver = declarations_->classes.at(parent.type.name).declaration;
  } else if (builtin != model_.builtin_method_values.end()) {
    plan.target =
        function_ids_.at(model_.generated_functions.at(builtin->second).declaration->name);
  } else if (interface != model_.interface_members.end()) {
    const auto& member = interface->second;
    plan.target =
        method_selectors_.at(QualifiedMemberKey({.owner = member.owner, .name = member.member}));
    plan.mode = CallableMode::VirtualMethod;
    plan.virtual_signature = &model_.interfaces.at(member.owner).methods.at(member.member);
  } else {
    const auto& member = model_.instance_members.at(&expression);
    const auto selected = ResolveMethodCall(
        expression, QualifiedMemberKey({.owner = member.owner, .name = member.member}));
    plan.target = selected.index;
    if (selected.opcode == Opcode::CallVirtual) {
      plan.mode = CallableMode::VirtualMethod;
      plan.virtual_signature = &model_.classes.at(member.owner).methods.at(member.member);
    }
  }
  if (super == model_.super_members.end() && plan.mode == CallableMode::BoundMethod) {
    plan.binding_receiver = ReceiverDeclaration(receiver.type);
  }
  return plan;
}

Lowerer::LoweredValue Lowerer::LowerFunctionValue(const frontend::Expression& expression) {
  const auto plan = ResolveFunctionValue(expression);
  const auto& receiver = plan.receiver;
  const std::array receiver_captures{
      receiver ? MakeReceiverCapture(receiver->type) : frontend::Capture{},
  };
  const auto captures =
      receiver ? std::span<const frontend::Capture>(receiver_captures) : plan.captures;
  const auto mode = plan.mode;
  const auto layout = CallableClass(plan.target, mode, captures, plan.virtual_signature);
  const auto object = AllocateRegister(expression.location);
  std::optional<std::size_t> null_jump;
  if (receiver && expression.null_safe) {
    null_jump = BeginNullSafe(receiver->reg, expression.location);
  }
  Emit({
      .opcode = Opcode::NewInstance,
      .dst = object,
      .index = layout,
      .location = expression.location,
  });
  if (receiver && mode == CallableMode::BoundMethod) {
    current_function_->instructions.back().binding_receiver = plan.binding_receiver;
  }
  for (std::size_t index = 0; index < captures.size(); ++index) {
    const auto reg = receiver ? receiver->reg : binding_registers_.at(captures[index].binding);
    Emit({
        .opcode = Opcode::SetField,
        .dst = object,
        .lhs = reg,
        .index = static_cast<FieldId>(index),
        .location = expression.location,
    });
  }
  FinishNullSafe(null_jump, object, expression.location);
  return {.reg = object, .type = model_.expression_types.at(&expression)};
}

Lowerer::LoweredValue Lowerer::LowerIndirectCall(const frontend::Expression& expression) {
  const auto callee = LowerExpression(*expression.callee);
  // callee 也可能是可重写的局部变量，必须在求值实参前保存。
  const auto callable = AllocateRegister(expression.location);
  Emit({
      .opcode = Opcode::Move,
      .dst = callable,
      .lhs = callee.reg,
      .location = expression.location,
  });
  const auto count = expression.arguments.size();
  const auto base = AllocateRegisterWindow(count, expression.location);
  LowerArguments(expression, base);
  const auto result = AllocateRegister(expression.location);
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
  std::optional<std::size_t> null_jump;
  if (expression.callee->null_safe) {
    null_jump = BeginNullSafe(receiver.reg, expression.location);
  }
  auto result = LowerInvocation(
      expression, signature,
      method_selectors_.at(QualifiedMemberKey({.owner = member.owner, .name = member.member})),
      &receiver, Opcode::CallVirtual);
  FinishNullSafe(null_jump, result.reg, expression.location);
  result.type = model_.expression_types.at(&expression);
  return result;
}

}  // namespace vela::ir
