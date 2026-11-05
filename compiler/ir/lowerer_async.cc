#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "compiler/frontend/builtins/async_builtins.h"

namespace vela::ir {

void Lowerer::RegisterAsyncMember(const frontend::TypeDescriptor& receiver, std::string_view name) {
  using Kind = frontend::TypeDescriptor::Kind;
  const auto* member =
      frontend::FindAsyncMember(receiver.kind, frontend::MemberStorage::Instance, name);
  if (member == nullptr) {
    return;
  }
  const auto operation = member->operation;
  const frontend::TypeDescriptor dynamic{.kind = Kind::Dynamic, .name = frontend::kDynamicTypeName};
  const std::array parameters{dynamic, dynamic};
  const auto count = static_cast<std::size_t>(std::ranges::count_if(
      member->parameters, [](std::string_view parameter) { return !parameter.empty(); }));
  auto signature =
      frontend::MakeAsyncSignature(*member, dynamic, std::span(parameters).first(count));
  if (operation == AsyncOperation::CompleterComplete &&
      model_.generic_arguments.at(receiver.name).front().kind == Kind::Void) {
    signature.optional_parameters.insert(std::string(member->parameters.at(0)));
  }
  auto function = CreateMemberFunction(receiver, name, signature);
  current_function_ = &function;
  current_handler_.reset();
  next_register_ = function.parameter_count;
  const auto empty = EmitTemporary({.opcode = Opcode::LoadNull, .location = {}});
  const auto selector = EmitTemporary({
      .opcode = Opcode::LoadInt,
      .immediate = static_cast<std::int64_t>(operation),
      .location = {},
  });
  const auto metadata = EmitTemporary({
      .opcode = Opcode::LoadInt,
      .immediate = operation == AsyncOperation::Timeout
                       ? static_cast<std::int64_t>(RegisterRuntimeType(
                             model_.types.at(frontend::kTimeoutExceptionTypeName)))
                       : -1,
      .location = {},
  });
  const std::array<RegisterId, 5> arguments{
      0,
      signature.parameters.empty() ? empty : 1U,
      signature.parameters.size() < 2 ? empty : 2U,
      selector,
      metadata,
  };
  const auto result = AllocateTemporary({});
  EmitBuiltinCall(Builtin::DynamicAsync, result, arguments, {});
  Emit({.opcode = Opcode::Return, .dst = result, .location = {}});
  function.register_count = next_register_;
  PublishMemberFunction(receiver, name, member->kind, std::move(function));
}

void Lowerer::LowerAsyncArguments(const frontend::Expression& expression,
                                  AsyncOperation operation,
                                  RegisterId base,
                                  const std::optional<LoweredValue>& receiver) {
  if (expression.kind != frontend::Expression::Kind::Call) {
    return;
  }
  const auto hidden = static_cast<RegisterId>(receiver.has_value());
  const frontend::TypeDescriptor* payload = nullptr;
  if (operation == AsyncOperation::CompleterComplete) {
    if (!receiver) {
      std::unreachable();
    }
    const auto& element = model_.generic_arguments.at(receiver->type.name).front();
    payload = &model_.types.at(std::string("Future<") + std::string(element.name) + ">");
  }
  const auto& order = model_.argument_order.at(&expression);
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    const auto& argument = expression.arguments.at(index);
    auto value = LowerExpression(*argument.value);
    if (operation == AsyncOperation::CompleterComplete) {
      value.reg = LowerFuturePayload(value, *payload, argument.location);
    }
    Emit({
        .opcode = Opcode::Move,
        .dst = base + hidden + order.at(index),
        .lhs = value.reg,
        .location = argument.location,
    });
  }
}

Lowerer::LoweredValue Lowerer::LowerAsyncCall(const frontend::Expression& expression) {
  const auto& descriptor = model_.async_calls.at(&expression);
  const auto operation = descriptor.operation;
  const auto count = AsyncArity(operation);
  std::optional<LoweredValue> receiver;
  const auto& member =
      expression.kind == frontend::Expression::Kind::Call ? *expression.callee : expression;
  if (descriptor.receiver) {
    receiver = LowerExpression(*member.object);
  }
  std::optional<std::size_t> null_jump;
  if (receiver && member.null_safe) {
    null_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfNull, .lhs = receiver->reg, .location = expression.location});
  }
  const auto base = next_register_;
  for (std::size_t index = 0; index < count; ++index) {
    const auto reg = AllocateRegister(expression.location);
    Emit({.opcode = Opcode::LoadNull, .dst = reg, .location = expression.location});
  }
  if (receiver) {
    Emit({
        .opcode = Opcode::Move,
        .dst = base,
        .lhs = receiver->reg,
        .location = expression.location,
    });
  }
  if (operation == AsyncOperation::Wait) {
    Emit({
        .opcode = Opcode::LoadBool,
        .dst = base + 1,
        .immediate = 0,
        .location = expression.location,
    });
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = base + 3,
        .immediate = -1,
        .location = expression.location,
    });
  }
  LowerAsyncArguments(expression, operation, base, receiver);
  if (descriptor.runtime_type) {
    const auto type_index = operation == AsyncOperation::Wait
                                ? RegisterArrayType(*descriptor.runtime_type)
                                : RegisterRuntimeType(*descriptor.runtime_type);
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = base + static_cast<RegisterId>(count - 1),
        .immediate = type_index,
        .location = expression.location,
    });
  }
  const auto result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::AsyncCall,
      .dst = result,
      .lhs = base,
      .index = static_cast<std::uint32_t>(operation),
      .location = expression.location,
  });
  FinishNullSafe(null_jump, result, expression.location);
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

}  // namespace vela::ir
