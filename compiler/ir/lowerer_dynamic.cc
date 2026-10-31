#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/member.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/frontend/type_names.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

void Lowerer::RegisterClassMembers(const frontend::Class& source,
                                   const frontend::ClassDescriptor& descriptor) {
  const auto receiver = RegisterRuntimeType(descriptor.type);
  for (const auto& name : descriptor.field_order) {
    const auto& field = descriptor.fields.at(name);
    if (field.visibility != frontend::Visibility::Public) {
      continue;
    }
    program_.members.push_back({
        .receiver_type = receiver,
        .name = name,
        .kind = MemberKind::Field,
        .target = field.index,
        .type = RegisterRuntimeType(field.type),
    });
  }
  std::vector<std::string> names;
  for (const auto& [name, signature] : descriptor.methods) {
    static_cast<void>(signature);
    names.push_back(name);
  }
  std::ranges::sort(names);
  for (const auto& name : names) {
    const auto& signature = descriptor.methods.at(name);
    if (name == "$constructor" || signature.storage != frontend::MemberStorage::Instance) {
      continue;
    }
    const auto owner = FindMethodOwner(source.name, name);
    if (signature.is_getter || signature.is_setter) {
      RegisterClassProperty(receiver, *FindClass(owner), name);
      continue;
    }
    auto key = owner;
    key += '.';
    key += name;
    const auto target = function_ids_.find(key);
    if (signature.visibility != frontend::Visibility::Public || target == function_ids_.end()) {
      continue;
    }
    const std::vector<frontend::Capture> captures{
        {
            .name = "this",
            .type = descriptor.type,
            .binding = 0,
            .kind = frontend::BindingKind::Constant,
        },
    };
    const auto wrapper = CallableClass(target->second, CallableMode::BoundMethod, captures,
                                       frontend::MakeFunctionType(signature));
    program_.members.push_back(
        {.receiver_type = receiver, .name = name, .kind = MemberKind::Method, .target = wrapper});
  }
}

void Lowerer::RegisterClassProperty(std::uint32_t receiver,
                                    const frontend::ClassDescriptor& owner,
                                    const std::string& name) {
  MemberDefinition property{
      .receiver_type = receiver,
      .name = name,
      .kind = MemberKind::Property,
      .target = kInvalidFunctionTarget,
  };
  for (const auto& method : owner.declaration->methods) {
    if (method.name != name || method.visibility != frontend::Visibility::Public) {
      continue;
    }
    auto key = std::string(owner.type.name);
    key += method.is_getter ? ".$get." : ".$set.";
    key += name;
    if (method.is_getter) {
      property.target = function_ids_.at(key);
    } else if (method.is_setter) {
      property.setter = function_ids_.at(key);
    }
  }
  if (property.target != kInvalidFunctionTarget || property.setter) {
    program_.members.push_back(std::move(property));
  }
}

Lowerer::LoweredValue Lowerer::LowerDynamicCall(const frontend::Expression& expression) {
  const auto location = expression.location;
  const auto value = LowerExpression(*expression.callee);
  const auto callable =
      EmitTemporary({.opcode = Opcode::Move, .lhs = value.reg, .location = location});
  const auto [arguments, names] = LowerDynamicArguments(expression);
  const auto result = AllocateTemporary(location);
  const std::array values{callable, arguments, names};
  EmitBuiltinCall(Builtin::DynamicCall, result, values, location);
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

std::pair<RegisterId, RegisterId> Lowerer::LowerDynamicArguments(
    const frontend::Expression& expression) {
  const auto location = expression.location;
  const frontend::TypeDescriptor dynamic{
      .kind = frontend::TypeDescriptor::Kind::Dynamic,
      .name = frontend::kDynamicTypeName,
  };
  const frontend::TypeDescriptor string{
      .kind = frontend::TypeDescriptor::Kind::String,
      .name = frontend::kStringTypeName,
  };
  const auto count = static_cast<std::int64_t>(expression.arguments.size());
  const auto arguments = EmitTemporary({
      .opcode = Opcode::NewArray,
      .immediate = count,
      .index = RegisterArrayType(frontend::MakeArrayType(dynamic)),
      .location = location,
  });
  const auto names = EmitTemporary({
      .opcode = Opcode::NewArray,
      .immediate = count,
      .index = RegisterArrayType(frontend::MakeArrayType(string)),
      .location = location,
  });
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    const auto& argument = expression.arguments.at(index);
    const auto actual = LowerExpression(*argument.value);
    const auto position = EmitTemporary({
        .opcode = Opcode::LoadInt,
        .immediate = static_cast<std::int64_t>(index),
        .location = argument.location,
    });
    Emit({
        .opcode = Opcode::SetIndex,
        .dst = arguments,
        .lhs = position,
        .rhs = actual.reg,
        .location = argument.location,
    });
    frontend::Expression name;
    name.location = argument.location;
    name.text = argument.name;
    const auto text = LowerStringLiteral(name);
    Emit({
        .opcode = Opcode::SetIndex,
        .dst = names,
        .lhs = position,
        .rhs = text.reg,
        .location = argument.location,
    });
  }
  return {arguments, names};
}

Lowerer::LoweredValue Lowerer::LowerDynamicMember(const frontend::Expression& expression,
                                                  Builtin operation) {
  const auto receiver = LowerExpression(*expression.object);
  const auto location = expression.location;
  const auto result = AllocateTemporary(location);
  std::optional<std::size_t> null_jump;
  if (expression.null_safe) {
    null_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfNull, .lhs = receiver.reg, .location = location});
  }
  frontend::Expression name;
  name.location = location;
  name.text = expression.text;
  const auto text = LowerStringLiteral(name);
  const std::array values{receiver.reg, text.reg};
  EmitBuiltinCall(operation, result, values, location);
  FinishNullSafe(null_jump, result, location);
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

Lowerer::LoweredValue Lowerer::LowerDynamicMethodCall(const frontend::Expression& expression) {
  const auto& callee = *expression.callee;
  const auto plan = LowerDynamicMember(callee, Builtin::DynamicResolveCall);
  const auto location = expression.location;
  const auto result = AllocateTemporary(location);
  std::optional<std::size_t> null_jump;
  if (callee.null_safe) {
    null_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfNull, .lhs = plan.reg, .location = location});
  }
  const auto [arguments, names] = LowerDynamicArguments(expression);
  const std::array values{plan.reg, arguments, names};
  EmitBuiltinCall(Builtin::DynamicMethodCall, result, values, location);
  FinishNullSafe(null_jump, result, location);
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

Lowerer::LoweredValue Lowerer::LowerDynamicAssignment(const frontend::Expression& expression) {
  const auto& member = *expression.left;
  const auto receiver = SnapshotBefore(LowerExpression(*member.object), *expression.right);
  frontend::Expression name;
  name.location = member.location;
  name.text = member.text;
  const auto text = LowerStringLiteral(name);
  const auto value = LowerExpression(*expression.right);
  const auto result = AllocateTemporary(expression.location);
  const std::array values{receiver.reg, text.reg, value.reg};
  EmitBuiltinCall(Builtin::DynamicMemberSet, result, values, expression.location);
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

Lowerer::LoweredValue Lowerer::LowerDynamicCompoundAssignment(
    const frontend::Expression& expression,
    Opcode operation) {
  auto target = LowerMutationTarget(*expression.left);
  target.value = SnapshotBefore(target.value, *expression.right);
  const auto right = LowerExpression(*expression.right);
  const auto selector = EmitTemporary({
      .opcode = Opcode::LoadInt,
      .immediate = static_cast<std::int64_t>(operation),
      .location = expression.location,
  });
  const auto result = AllocateTemporary(expression.location);
  const std::array values{selector, target.value.reg, right.reg};
  EmitBuiltinCall(Builtin::DynamicBinary, result, values, expression.location);
  StoreMutationTarget(*expression.left, target, result, expression.location);
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

RegisterId Lowerer::LowerFuturePayload(const LoweredValue& value,
                                       const frontend::TypeDescriptor& future,
                                       frontend::SourceLocation location) {
  using Kind = frontend::TypeDescriptor::Kind;
  if (value.type.kind == Kind::Enum) {
    return LowerConversion(value, {.kind = Kind::Dynamic, .name = frontend::kDynamicTypeName},
                           location)
        .reg;
  }
  if (value.type.kind != Kind::Dynamic) {
    return value.reg;
  }
  const auto identity = EmitTemporary({
      .opcode = Opcode::LoadInt,
      .immediate = RegisterRuntimeType(future),
      .location = location,
  });
  const auto result = AllocateTemporary(location);
  const std::array arguments{value.reg, identity};
  EmitBuiltinCall(Builtin::CheckFuturePayload, result, arguments, location);
  return result;
}

}  // namespace vela::ir
