#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
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
  const auto value = AllocateTemporary(location);
  RecordValueType(value, local.type);
  Emit({.opcode = Opcode::GetField, .dst = value, .lhs = local.reg, .location = location});
  return {.reg = value, .type = local.type};
}

ClassId Lowerer::CallableClass(std::uint32_t function_id,
                               CallableMode mode,
                               const std::vector<frontend::Capture>& captures,
                               const frontend::FunctionDescriptor* interface_signature) {
  const auto key = (static_cast<std::uint64_t>(mode) << 32U) | function_id;
  const auto existing = callable_classes_.find(key);
  if (existing != callable_classes_.end()) {
    return existing->second;
  }
  const auto layout_id = static_cast<ClassId>(program_.classes.size());
  Class layout;
  layout.name = "$callable" + std::to_string(key);
  layout.callable = CallableTarget{.function = function_id, .mode = mode};
  if (interface_signature != nullptr) {
    auto& signature = layout.callable_type.emplace();
    for (const auto& parameter : interface_signature->parameters) {
      signature.parameters.push_back({
          .name = parameter.name,
          .type = RegisterRuntimeType(parameter.type),
          .optional = interface_signature->optional_parameters.contains(parameter.name),
          .rest = parameter.rest,
      });
    }
    if (interface_signature->return_type.kind != frontend::TypeDescriptor::Kind::Void) {
      signature.result = RegisterRuntimeType(interface_signature->return_type);
    }
  }
  for (const auto& capture : captures) {
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

Lowerer::FunctionValuePlan Lowerer::ResolveFunctionValue(const frontend::Expression& expression) {
  const bool lambda = expression.kind == frontend::Expression::Kind::Lambda;
  const auto interface = model_.interface_members.find(&expression);
  const auto static_member = model_.static_members.find(&expression);
  const auto builtin = model_.builtin_method_values.find(&expression);
  const auto super = model_.super_members.find(&expression);
  const auto instance = model_.instance_members.find(&expression);
  const bool method = expression.kind == frontend::Expression::Kind::Member &&
                      static_member == model_.static_members.end();
  FunctionValuePlan plan;
  auto& captures = plan.captures;
  auto& target = plan.target;
  auto& receiver = plan.receiver;
  if (lambda) {
    const auto index = model_.lambda_indices.at(&expression);
    target = lambda_ids_.at(index);
    captures = model_.lambdas.at(index).captures;
  } else if (method) {
    if (super != model_.super_members.end()) {
      const auto* self = FindLocal(frontend::kReceiverName);
      receiver = ReadLocal(*self, expression.location);
      target = function_ids_.at(super->second.owner + "." + super->second.member);
    } else {
      receiver = LowerExpression(*expression.object);
      if (builtin != model_.builtin_method_values.end()) {
        target = function_ids_.at(model_.generated_functions.at(builtin->second).declaration->name);
      } else if (interface != model_.interface_members.end()) {
        target = interface_method_ids_.at(interface->second.owner + "." + interface->second.member);
      } else {
        target = function_ids_.at(instance->second.owner + "." + instance->second.member);
      }
    }
    captures.push_back({
        .name = std::string(frontend::kReceiverName),
        .type = receiver->type,
        .binding = 0,
        .kind = frontend::BindingKind::Constant,
    });
  } else if (static_member != model_.static_members.end()) {
    target = function_ids_.at(static_member->second.owner + "." + static_member->second.member);
  } else {
    const auto application = model_.generic_applications.find(&expression);
    target = function_ids_.at(
        application == model_.generic_applications.end() ? expression.text : application->second);
  }
  if (lambda) {
    plan.mode = CallableMode::Closure;
  } else if (interface != model_.interface_members.end()) {
    plan.mode = CallableMode::InterfaceMethod;
  } else if (method) {
    plan.mode = CallableMode::BoundMethod;
  }
  return plan;
}

Lowerer::LoweredValue Lowerer::LowerFunctionValue(const frontend::Expression& expression) {
  const auto plan = ResolveFunctionValue(expression);
  const auto& receiver = plan.receiver;
  const auto& captures = plan.captures;
  const auto mode = plan.mode;
  const auto interface = model_.interface_members.find(&expression);
  const auto* signature =
      interface == model_.interface_members.end()
          ? nullptr
          : &model_.interfaces.at(interface->second.owner).methods.at(interface->second.member);
  const auto layout = CallableClass(plan.target, mode, captures, signature);
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
  if (declarations_ != nullptr && receiver && mode == CallableMode::BoundMethod) {
    const auto& type = receiver->type;
    auto name = std::string(type.name);
    if (model_.super_members.contains(&expression)) {
      const auto& parent = model_.classes.at(name).base_class;
      if (!parent) {
        std::unreachable();
      }
      name = *parent;
    } else if (type.nullable) {
      name.resize(name.size() - 1);
    }
    const auto owner = declarations_->classes.find(name);
    if (owner != declarations_->classes.end()) {
      current_function_->instructions.back().binding_receiver = owner->second.declaration;
    }
  }
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
  FinishNullSafe(null_jump, object, expression.location);
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
  const auto base = AllocateRegisterWindow(count, expression.location);
  LowerArguments(expression, base);
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
