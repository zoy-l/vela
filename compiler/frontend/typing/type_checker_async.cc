#include "base/async_operation.h"
#include "base/member.h"
#include "compiler/frontend/builtins/async_builtins.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/frontend/typing/type_inference.h"
#include "compiler/frontend/typing/type_names.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vela::frontend {
namespace {

using Type = TypeDescriptor;
using Kind = Type::Kind;

const Type kVoid{.kind = Kind::Void, .name = kVoidTypeName};
const Type kInt{.kind = Kind::Int, .name = kIntTypeName};
const Type kBool{.kind = Kind::Bool, .name = kBoolTypeName};

std::optional<std::size_t> Argument(const Expression& expression,
                                    std::string_view name,
                                    std::size_t position) {
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    const auto& argument = expression.arguments.at(index);
    if (argument.name == name || (argument.name.empty() && index == position)) {
      return index;
    }
  }
  return std::nullopt;
}

bool IsThrowable(const Type& type) {
  return type.kind == Kind::Dynamic ||
         (!type.nullable && (type.kind == Kind::Class || type.kind == Kind::Interface));
}

}  // namespace

TypeChecker::Type TypeChecker::RegisterAsyncCall(const Expression& expression,
                                                 AsyncCallDescriptor descriptor,
                                                 std::span<const std::optional<Type>> checked) {
  const auto result = CheckFunctionCall(descriptor.signature, expression, checked);
  facts_.async_calls.insert_or_assign(&expression, std::move(descriptor));
  return result;
}

TypeChecker::Type TypeChecker::MakeFutureType(const Expression& expression, const Type& element) {
  if (!IsValid(element)) {
    return InvalidType();
  }
  if (element.kind == Kind::Future) {
    if (!element.nullable) {
      return element;
    }
    auto value = generic_arguments_.at(NonNullable(element).name).front();
    if (value.kind != Kind::Void) {
      value = NullableType(value);
    }
    return MakeFutureType(expression, value);
  }
  return ResolveGenericType({
      .name = std::string(kFutureTypeName),
      .location = expression.location,
      .kind = TypeName::Kind::Generic,
      .arguments =
          {
              {
                  .name = std::string(element.name),
                  .location = expression.location,
                  .kind = TypeName::Kind::Named,
                  .arguments = {},
                  .parameter = {},
              },
          },
      .parameter = {},
  });
}

TypeChecker::Type TypeChecker::CheckAsyncCallback(const Expression& expression,
                                                  std::span<const Type> parameters,
                                                  CallbackPresence presence,
                                                  CallbackParameters contract) {
  FunctionInfo context;
  for (const auto& type : parameters) {
    context.AddParameter(std::to_string(context.parameters.size()), type);
  }
  return CheckCallback(expression, std::move(context), CallbackArity::Exact, presence, contract);
}

TypeChecker::Type TypeChecker::CheckFutureCall(const Expression& expression, const Type* expected) {
  const auto& method = expression.callee->text;
  if (method == "wait" || method == "race") {
    return CheckFutureAggregation(expression, expected);
  }
  if (method == "sync" || method == "microtask" || method == "delayed") {
    return CheckFutureComputation(expression, expected);
  }
  return CheckFutureValueFactory(expression, expected);
}

TypeChecker::Type TypeChecker::CheckFutureValueFactory(const Expression& expression,
                                                       const Type* expected) {
  const auto& callee = *expression.callee;
  if (callee.text != "value" && callee.text != "error") {
    AddDiagnostic(callee.location, "unknown Future factory '" + callee.text + "'");
    return InvalidType();
  }
  auto inference = CreateFutureInference(expression, expected);
  if (!inference) {
    return InvalidType();
  }
  if (expression.arguments.size() != 1) {
    AddDiagnostic(expression.location, "Future factory expects one argument");
    return InvalidType();
  }
  const auto element =
      ResolveInferenceContext(IntrinsicTypeVariable("$Value", expression.location), *inference);
  const auto argument =
      CheckExpression(*expression.arguments.front().value,
                      callee.text == "value" && IsValid(element) ? &element : nullptr);
  const auto signature = MakeAsyncSignature(
      *FindAsyncMember(Kind::Future, MemberStorage::Static, callee.text), {}, std::array{argument});
  const std::array<std::optional<Type>, 1> checked{argument};
  static_cast<void>(CheckFunctionCall(signature, expression, checked));
  if (callee.text == "error") {
    if (!IsThrowable(argument)) {
      AddDiagnostic(expression.location, "Future.error requires a non-null throwable object");
    }
    return InferFutureResult(expression, *inference, nullptr);
  }
  return InferFutureResult(expression, *inference, &argument);
}

TypeChecker::Type TypeChecker::CheckFutureComputation(const Expression& expression,
                                                      const Type* expected) {
  const auto& callee = *expression.callee;
  const auto& method = callee.text;
  auto inference = CreateFutureInference(expression, expected);
  if (!inference) {
    return InvalidType();
  }
  const bool delayed = method == "delayed";
  const auto callback = Argument(expression, "computation", delayed ? 1 : 0);
  if (!callback) {
    AddDiagnostic(expression.location, "Future factory requires a computation");
    return InvalidType();
  }
  auto type = CheckAsyncCallback(*expression.arguments.at(*callback).value, {});
  if (!IsValid(type)) {
    return type;
  }
  const auto produced = function_types_.at(type.name).return_type;
  const auto result = InferFutureResult(expression, *inference, &produced);
  std::vector<Type> parameters;
  if (delayed) {
    parameters.push_back(kInt);
  }
  parameters.push_back(type);
  auto signature = MakeAsyncSignature(*FindAsyncMember(Kind::Future, MemberStorage::Static, method),
                                      result, parameters);
  std::vector<std::optional<Type>> checked(expression.arguments.size());
  checked.at(*callback) = type;
  if (method == "sync") {
    return CheckFunctionCall(signature, expression, checked);
  }
  return RegisterAsyncCall(
      expression,
      {
          .operation = delayed ? AsyncOperation::Delayed : AsyncOperation::Microtask,
          .signature = std::move(signature),
      },
      checked);
}

void TypeChecker::AddFutureCleanup(const Expression& expression,
                                   FunctionInfo& signature,
                                   const Type& element,
                                   std::vector<std::optional<Type>>& checked) {
  FunctionInfo context;
  context.return_type = kVoid;
  if (element.kind != Kind::Void) {
    context.AddParameter("value", element);
  }
  auto callback_type = NullableType(FunctionType(context));
  const auto input = Argument(expression, "cleanUp", 2);
  if (input && expression.arguments.at(*input).value->kind != Expression::Kind::NullLiteral) {
    const std::array<Type, 1> parameters{element};
    auto type = CheckAsyncCallback(
        *expression.arguments.at(*input).value,
        std::span<const Type>(parameters).first(element.kind == Kind::Void ? 0 : 1),
        CallbackPresence::Optional);
    if (IsValid(type)) {
      const auto& callback = function_types_.at(NonNullable(type).name);
      if (callback.parameters.size() == 1 &&
          !CanAssign(callback.parameters.front().type, element)) {
        AddDiagnostic(expression.location, "cleanUp parameter must accept the input element type");
      }
      if (callback.return_type.kind != Kind::Void &&
          !IsSameType(callback.return_type, MakeFutureType(expression, kVoid))) {
        AddDiagnostic(expression.location, "cleanUp must return void or Future<void>");
      }
      callback_type = NullableType(type);
      checked.at(*input) = type;
    }
  }
  signature = MakeAsyncSignature(
      *FindAsyncMember(Kind::Future, MemberStorage::Static, "wait"), signature.return_type,
      std::array{signature.parameters.front().type, kBool, callback_type});
}

TypeChecker::Type TypeChecker::ProjectFutureAggregationContext(const Expression& expression,
                                                               const Type* expected) {
  auto result = expected != nullptr ? NonNullable(*expected) : InvalidType();
  if (expression.callee->text != "wait" || result.kind != Kind::Future) {
    return result;
  }
  const auto payload = generic_arguments_.at(result.name).front();
  if (payload.kind == Kind::Array) {
    return MakeFutureType(expression, array_elements_.at(payload.name));
  }
  return payload.kind == Kind::Void ? result : InvalidType();
}

TypeChecker::Type TypeChecker::ProjectFutureArrayElement(const Expression& expression,
                                                         const Type& values) {
  if (!IsValid(values) || values.kind == Kind::Dynamic) {
    return InvalidType();
  }
  if (values.kind != Kind::Array || values.nullable) {
    AddDiagnostic(expression.location, "Future aggregation expects Future<T>[]");
    return InvalidType();
  }
  const auto future = array_elements_.at(values.name);
  if (future.kind != Kind::Future || future.nullable) {
    AddDiagnostic(expression.location, "Future aggregation expects non-null Future<T> elements");
    return InvalidType();
  }
  return generic_arguments_.at(future.name).front();
}

TypeChecker::Type TypeChecker::CheckFutureAggregation(const Expression& expression,
                                                      const Type* expected) {
  const bool wait = expression.callee->text == "wait";
  const auto input = Argument(expression, "values", 0);
  if (!input) {
    AddDiagnostic(expression.location, "Future aggregation requires values");
    return InvalidType();
  }
  const auto expected_result = ProjectFutureAggregationContext(expression, expected);
  auto inference =
      CreateFutureInference(expression, IsValid(expected_result) ? &expected_result : nullptr);
  if (!inference) {
    return InvalidType();
  }
  const auto variable = IntrinsicTypeVariable("$Value", expression.location);
  auto element = ResolveInferenceContext(variable, *inference);
  const auto context =
      IsValid(element) ? ArrayType(MakeFutureType(expression, element)) : InvalidType();
  const auto values = CheckExpression(*expression.arguments.at(*input).value,
                                      IsValid(context) ? &context : nullptr);
  const auto actual = ProjectFutureArrayElement(expression, values);
  if (IsValid(actual)) {
    inference->Constrain(variable, DescribeType(actual), TypeInference::Evidence::Argument);
  }
  element = ResolveInferenceContext(variable, *inference);
  if (!IsValid(element)) {
    AddDiagnostic(expression.location, "cannot infer Future aggregation element type");
    return InvalidType();
  }
  const auto collected = wait && element.kind != Kind::Void ? ArrayType(element) : element;
  auto signature = MakeAsyncSignature(
      *FindAsyncMember(Kind::Future, MemberStorage::Static, expression.callee->text),
      MakeFutureType(expression, collected),
      std::array{ArrayType(MakeFutureType(expression, element))});
  std::vector<std::optional<Type>> checked(expression.arguments.size());
  checked.at(*input) = values;
  if (wait) {
    AddFutureCleanup(expression, signature, element, checked);
  }
  return RegisterAsyncCall(
      expression,
      {
          .operation = wait ? AsyncOperation::Wait : AsyncOperation::Race,
          .signature = std::move(signature),
          .runtime_type =
              wait && element.kind != Kind::Void ? std::optional(collected) : std::nullopt,
      },
      checked);
}

TypeChecker::Type TypeChecker::CheckAsyncConstruction(const Expression& expression,
                                                      const Type* expected) {
  Type type;
  AsyncOperation operation = AsyncOperation::Count;
  if (expression.callee->text == kCompleterTypeName) {
    operation = AsyncOperation::NewCompleter;
    const std::array parameters{IntrinsicTypeParameter("$Value")};
    type = InferConstruction(expression, expected, kCompleterTypeName, parameters);
  } else {
    operation = AsyncOperation::NewCancellationToken;
    type = types_.at(kCancellationTokenTypeName);
  }
  FunctionInfo signature;
  signature.return_type = type;
  return RegisterAsyncCall(expression, {.operation = operation, .signature = std::move(signature)});
}

TypeChecker::Type TypeChecker::CheckTimerFactory(const Expression& expression) {
  const auto& method = expression.callee->text;
  const auto* contract = FindAsyncMember(Kind::Timer, MemberStorage::Static, method);
  if (contract == nullptr) {
    AddDiagnostic(expression.location, "unknown Timer factory");
    return InvalidType();
  }
  const auto callback = Argument(expression, "callback", 1);
  if (!callback) {
    AddDiagnostic(expression.location, "Timer factory requires a callback");
    return InvalidType();
  }
  auto type = CheckAsyncCallback(*expression.arguments.at(*callback).value, {});
  if (!IsValid(type)) {
    return type;
  }
  const auto returned = function_types_.at(type.name).return_type;
  if (returned.kind != Kind::Void && !IsSameType(returned, MakeFutureType(expression, kVoid))) {
    AddDiagnostic(expression.location, "Timer callback must return void or Future<void>");
  }
  auto signature = MakeAsyncSignature(*contract, types_.at(kTimerTypeName), std::array{kInt, type});
  std::vector<std::optional<Type>> checked(expression.arguments.size());
  checked.at(*callback) = type;
  return RegisterAsyncCall(expression,
                           {
                               .operation = std::get<AsyncOperation>(contract->operation),
                               .signature = std::move(signature),
                           },
                           checked);
}

TypeChecker::Type TypeChecker::CheckAsyncProperty(const Expression& expression,
                                                  const Type& receiver) {
  FunctionInfo signature;
  AsyncOperation operation = AsyncOperation::Count;
  if (receiver.kind == Kind::Completer && expression.text == "future") {
    signature.return_type =
        MakeFutureType(expression, generic_arguments_.at(receiver.name).front());
    operation = AsyncOperation::CompleterFuture;
  } else {
    signature.return_type = kBool;
    operation = receiver.kind == Kind::Completer ? AsyncOperation::CompleterIsCompleted
                                                 : AsyncOperation::CancellationIsCancelled;
  }
  const auto result = signature.return_type;
  signature = MakeAsyncSignature(
      *FindAsyncMember(receiver.kind, MemberStorage::Instance, expression.text), result, {});
  facts_.async_calls.insert_or_assign(&expression, AsyncCallDescriptor{
                                                       .operation = operation,
                                                       .signature = std::move(signature),
                                                       .receiver = true,
                                                   });
  return result;
}

TypeChecker::Type TypeChecker::CheckCompleterMethod(const Expression& expression,
                                                    const Type& receiver) {
  const bool error = expression.callee->text == "completeError";
  const auto element = generic_arguments_.at(receiver.name).front();
  static_cast<void>(MakeFutureType(expression, element));
  AsyncCallDescriptor descriptor{
      .operation = error ? AsyncOperation::CompleterError : AsyncOperation::CompleterComplete,
      .signature = {},
      .receiver = true,
  };
  auto& signature = descriptor.signature;
  signature.return_type = kVoid;
  std::vector<std::optional<Type>> checked(expression.arguments.size());
  if (error || element.kind != Kind::Void || !expression.arguments.empty()) {
    const auto input = Argument(expression, error ? "error" : "value", 0);
    Type type = element;
    if (input) {
      type = CheckExpression(*expression.arguments.at(*input).value, error ? nullptr : &element);
      checked.at(*input) = type;
      const bool valid = error ? IsThrowable(type)
                               : type.kind == Kind::Dynamic || CanAssign(element, type) ||
                                     IsSameType(MakeFutureType(expression, element), type);
      if (!valid) {
        AddDiagnostic(expression.location, "Completer payload does not match its contract");
      }
    }
    signature = MakeAsyncSignature(
        *FindAsyncMember(receiver.kind, MemberStorage::Instance, expression.callee->text), kVoid,
        std::array{type});
  }
  return RegisterAsyncCall(expression, std::move(descriptor), checked);
}

TypeChecker::Type TypeChecker::AddThenError(const Expression& expression,
                                            FunctionInfo& signature,
                                            std::vector<std::optional<Type>>& checked) {
  auto error_type = types_.at(kErrorTypeName);
  FunctionInfo context;
  context.return_type = generic_arguments_.at(signature.return_type.name).front();
  context.AddParameter("error", error_type);
  auto callback = NullableType(FunctionType(context));
  const auto input = Argument(expression, "onError", 1);
  if (input && expression.arguments.at(*input).value->kind != Expression::Kind::NullLiteral) {
    const std::array<Type, 1> parameters{error_type};
    auto type = CheckAsyncCallback(*expression.arguments.at(*input).value, parameters,
                                   CallbackPresence::Optional, CallbackParameters::ExceptionFilter);
    if (IsValid(type)) {
      const auto& actual = function_types_.at(NonNullable(type).name);
      if (actual.parameters.size() == 1) {
        error_type = actual.parameters.front().type;
      }
      if (!CanAssign(signature.return_type, MakeFutureType(expression, actual.return_type))) {
        AddDiagnostic(expression.location, "then onError result must match onValue");
      }
      callback = NullableType(type);
      checked.at(*input) = type;
    }
  }
  signature = MakeAsyncSignature(*FindAsyncMember(Kind::Future, MemberStorage::Instance, "then"),
                                 signature.return_type,
                                 std::array{signature.parameters.front().type, callback});
  return error_type;
}

TypeChecker::Type TypeChecker::CheckFutureCallback(const Expression& expression,
                                                   const Type& receiver) {
  const auto& method = expression.callee->text;
  const bool then = method == "then";
  const bool cleanup = method == "whenComplete";
  std::string_view name = "onError";
  auto operation = AsyncOperation::CatchError;
  if (then) {
    name = "onValue";
    operation = AsyncOperation::Then;
  }
  if (cleanup) {
    name = "action";
    operation = AsyncOperation::WhenComplete;
  }
  const auto input = Argument(expression, name, 0);
  if (!input) {
    AddDiagnostic(expression.location, "Future method requires a callback");
    return InvalidType();
  }
  const auto element = generic_arguments_.at(receiver.name).front();
  std::vector<Type> parameters;
  if (!then && !cleanup) {
    parameters.push_back(types_.at(kErrorTypeName));
  }
  if (then && element.kind != Kind::Void) {
    parameters.push_back(element);
  }
  auto callback = CheckAsyncCallback(
      *expression.arguments.at(*input).value, parameters, CallbackPresence::Required,
      !then && !cleanup ? CallbackParameters::ExceptionFilter : CallbackParameters::Assignable);
  if (!IsValid(callback)) {
    return callback;
  }
  const auto& callback_signature = function_types_.at(callback.name);
  const auto output = MakeFutureType(expression, callback_signature.return_type);
  if (!IsValid(output)) {
    return InvalidType();
  }
  AsyncCallDescriptor descriptor{.operation = operation, .signature = {}, .receiver = true};
  auto& signature = descriptor.signature;
  signature.return_type = then ? output : receiver;
  if (then && !parameters.empty() && callback_signature.parameters.size() == 1 &&
      !CanAssign(callback_signature.parameters.front().type, element)) {
    AddDiagnostic(expression.location, "then callback must accept the Future value type");
  }
  if (!then && !cleanup && !CanAssign(receiver, output)) {
    AddDiagnostic(expression.location, "catchError callback must return the Future value type");
  }
  if (cleanup && !IsSameType(output, MakeFutureType(expression, kVoid))) {
    AddDiagnostic(expression.location, "whenComplete action must return void or Future<void>");
  }
  signature = MakeAsyncSignature(*FindAsyncMember(receiver.kind, MemberStorage::Instance, method),
                                 signature.return_type, std::array{callback});
  std::vector<std::optional<Type>> checked(expression.arguments.size());
  checked.at(*input) = callback;
  if (!cleanup) {
    auto error_type = types_.at(kErrorTypeName);
    if (then) {
      error_type = AddThenError(expression, signature, checked);
    } else if (callback_signature.parameters.size() == 1) {
      error_type = callback_signature.parameters.front().type;
    }
    if (!IsThrowable(error_type)) {
      AddDiagnostic(expression.location, "error callback requires a non-null throwable parameter");
    }
    descriptor.runtime_type = error_type;
  }
  return RegisterAsyncCall(expression, std::move(descriptor), checked);
}

TypeChecker::Type TypeChecker::CheckFutureTimeout(const Expression& expression,
                                                  const Type& receiver) {
  const auto element = generic_arguments_.at(receiver.name).front();
  AsyncCallDescriptor descriptor{
      .operation = AsyncOperation::Timeout,
      .signature = {},
      .receiver = true,
      .runtime_type = types_.at(kTimeoutExceptionTypeName),
  };
  auto& signature = descriptor.signature;
  signature.return_type = receiver;
  FunctionInfo context;
  context.return_type = element;
  auto callback = NullableType(FunctionType(context));
  std::vector<std::optional<Type>> checked(expression.arguments.size());
  const auto input = Argument(expression, "onTimeout", 1);
  if (input && expression.arguments.at(*input).value->kind != Expression::Kind::NullLiteral) {
    const auto type =
        CheckAsyncCallback(*expression.arguments.at(*input).value, {}, CallbackPresence::Optional);
    if (IsValid(type)) {
      const auto output =
          MakeFutureType(expression, function_types_.at(NonNullable(type).name).return_type);
      if (!CanAssign(receiver, output)) {
        AddDiagnostic(expression.location, "onTimeout must return the Future value type");
      }
      callback = NullableType(type);
      checked.at(*input) = type;
    }
  }
  signature =
      MakeAsyncSignature(*FindAsyncMember(receiver.kind, MemberStorage::Instance, "timeout"),
                         receiver, std::array{kInt, callback});
  return RegisterAsyncCall(expression, std::move(descriptor), checked);
}

TypeChecker::Type TypeChecker::CheckAsyncMethod(const Expression& expression,
                                                const Type& receiver) {
  const auto& method = expression.callee->text;
  const auto* contract = FindAsyncMember(receiver.kind, MemberStorage::Instance, method);
  if (contract == nullptr || contract->kind != MemberKind::Method) {
    AddDiagnostic(expression.location, "unknown async method '" + method + "'");
    return InvalidType();
  }
  const auto operation = std::get<AsyncOperation>(contract->operation);
  switch (operation) {
    case AsyncOperation::CompleterComplete:
    case AsyncOperation::CompleterError:
      return CheckCompleterMethod(expression, receiver);
    case AsyncOperation::Timeout:
      return CheckFutureTimeout(expression, receiver);
    case AsyncOperation::Then:
    case AsyncOperation::CatchError:
    case AsyncOperation::WhenComplete:
      return CheckFutureCallback(expression, receiver);
    case AsyncOperation::TimerCancel:
    case AsyncOperation::CancellationCancel:
      return RegisterAsyncCall(expression,
                               {
                                   .operation = operation,
                                   .signature = MakeAsyncSignature(*contract, kVoid, {}),
                                   .receiver = true,
                               });
    default:
      std::unreachable();
  }
}

TypeChecker::Type TypeChecker::CheckSleepCall(const Expression& expression) {
  const auto signature =
      MakeAsyncSignature(*FindAsyncMember(Kind::Invalid, MemberStorage::Static, kSleepFunctionName),
                         MakeFutureType(expression, kVoid), std::array{kInt});
  return CheckFunctionCall(signature, expression);
}

}  // namespace vela::frontend
