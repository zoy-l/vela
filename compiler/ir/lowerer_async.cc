#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "base/member.h"

namespace vela::ir {
namespace {

enum class ParameterPresence : std::uint8_t { Required, Optional };

void AddDynamicParameter(frontend::FunctionDescriptor& signature,
                         std::string name,
                         ParameterPresence presence = ParameterPresence::Required) {
  if (presence == ParameterPresence::Optional) {
    signature.optional_parameters.insert(name);
  }
  signature.parameter_indices.emplace(name,
                                      static_cast<std::uint32_t>(signature.parameters.size()));
  signature.parameters.push_back({
      .name = std::move(name),
      .type = {.kind = frontend::TypeDescriptor::Kind::Dynamic, .name = "dynamic"},
  });
}

}  // namespace

void Lowerer::RegisterAsyncMember(const frontend::TypeDescriptor& receiver, std::string_view name) {
  using Kind = frontend::TypeDescriptor::Kind;
  struct Member final {
    Kind receiver = Kind::Invalid;
    std::string_view name;
    AsyncOperation operation = AsyncOperation::Count;
    MemberKind kind = MemberKind::Method;
    std::string_view first;
    std::string_view second;
  };
  static constexpr auto kMembers = std::to_array<Member>({
      {
          .receiver = Kind::Future,
          .name = "then",
          .operation = AsyncOperation::Then,
          .kind = MemberKind::Method,
          .first = "onValue",
          .second = "onError",
      },
      {
          .receiver = Kind::Future,
          .name = "catchError",
          .operation = AsyncOperation::CatchError,
          .kind = MemberKind::Method,
          .first = "onError",
          .second = {},
      },
      {
          .receiver = Kind::Future,
          .name = "whenComplete",
          .operation = AsyncOperation::WhenComplete,
          .kind = MemberKind::Method,
          .first = "action",
          .second = {},
      },
      {
          .receiver = Kind::Future,
          .name = "timeout",
          .operation = AsyncOperation::Timeout,
          .kind = MemberKind::Method,
          .first = "milliseconds",
          .second = "onTimeout",
      },
      {
          .receiver = Kind::Completer,
          .name = "future",
          .operation = AsyncOperation::CompleterFuture,
          .kind = MemberKind::Property,
          .first = {},
          .second = {},
      },
      {
          .receiver = Kind::Completer,
          .name = "isCompleted",
          .operation = AsyncOperation::CompleterIsCompleted,
          .kind = MemberKind::Property,
          .first = {},
          .second = {},
      },
      {
          .receiver = Kind::Completer,
          .name = "complete",
          .operation = AsyncOperation::CompleterComplete,
          .kind = MemberKind::Method,
          .first = "value",
          .second = {},
      },
      {
          .receiver = Kind::Completer,
          .name = "completeError",
          .operation = AsyncOperation::CompleterError,
          .kind = MemberKind::Method,
          .first = "error",
          .second = {},
      },
      {
          .receiver = Kind::Timer,
          .name = "cancel",
          .operation = AsyncOperation::TimerCancel,
          .kind = MemberKind::Method,
          .first = {},
          .second = {},
      },
      {
          .receiver = Kind::CancellationToken,
          .name = "cancel",
          .operation = AsyncOperation::CancellationCancel,
          .kind = MemberKind::Method,
          .first = {},
          .second = {},
      },
      {
          .receiver = Kind::CancellationToken,
          .name = "isCancelled",
          .operation = AsyncOperation::CancellationIsCancelled,
          .kind = MemberKind::Property,
          .first = {},
          .second = {},
      },
  });
  const auto member = std::ranges::find_if(kMembers, [&receiver, name](const auto& entry) {
    return entry.receiver == receiver.kind && entry.name == name;
  });
  if (member == kMembers.end()) {
    return;
  }
  const auto operation = member->operation;
  frontend::FunctionDescriptor signature;
  signature.return_type = {.kind = Kind::Dynamic, .name = "dynamic"};
  if (!member->first.empty()) {
    const bool optional = operation == AsyncOperation::CompleterComplete &&
                          model_.generic_arguments.at(receiver.name).front().kind == Kind::Void;
    AddDynamicParameter(signature, std::string(member->first),
                        optional ? ParameterPresence::Optional : ParameterPresence::Required);
  }
  if (!member->second.empty()) {
    AddDynamicParameter(signature, std::string(member->second), ParameterPresence::Optional);
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
      .immediate =
          operation == AsyncOperation::Timeout
              ? static_cast<std::int64_t>(RegisterRuntimeType(model_.types.at("TimeoutException")))
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
  PublishMemberFunction(receiver, name, member->kind, signature, std::move(function));
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
  const auto hidden = receiver ? 1U : 0U;
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
  if (expression.kind == frontend::Expression::Kind::Call) {
    const auto& order = model_.argument_order.at(&expression);
    for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
      const auto& argument = expression.arguments.at(index);
      auto value = LowerExpression(*argument.value);
      if (operation == AsyncOperation::CompleterComplete &&
          value.type.kind == frontend::TypeDescriptor::Kind::Dynamic) {
        const auto& element = model_.generic_arguments.at((*receiver).type.name).front();
        const auto& future = model_.types.at("Future<" + element.name + ">");
        value.reg = LowerFuturePayload(value, future, argument.location);
      }
      if (operation == AsyncOperation::CompleterComplete &&
          value.type.kind == frontend::TypeDescriptor::Kind::Enum) {
        value = LowerConversion(
            value, {.kind = frontend::TypeDescriptor::Kind::Dynamic, .name = "dynamic"},
            argument.location);
      }
      Emit({
          .opcode = Opcode::Move,
          .dst = base + hidden + order.at(index),
          .lhs = value.reg,
          .location = argument.location,
      });
    }
  }
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
  if (null_jump) {
    const auto end_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = expression.location});
    current_function_->instructions.at(*null_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    Emit({.opcode = Opcode::LoadNull, .dst = result, .location = expression.location});
    current_function_->instructions.at(end_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

}  // namespace vela::ir
