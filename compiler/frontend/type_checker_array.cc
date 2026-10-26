#include "base/array_algorithm.h"
#include "compiler/frontend/type_checker.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"

namespace vela::frontend {

TypeChecker::Type TypeChecker::CheckArrayCallback(const Expression& expression,
                                                  const FunctionInfo& expected) {
  Type actual;
  if (expression.kind == Expression::Kind::Lambda) {
    const auto count = expression.function->parameters.size();
    if (count > expected.parameters.size()) {
      AddDiagnostic(expression.location, "array callback declares too many parameters");
      return InvalidType();
    }
    auto context = expected;
    context.parameters.resize(count);
    const auto type = FunctionType(context);
    actual = CheckExpression(expression, &type);
    if (!IsValid(context.return_type) && type.name != actual.name) {
      function_types_.erase(type.name);
      types_.erase(type.name);
    }
  } else {
    actual = CheckExpression(expression, nullptr);
  }
  if (actual.kind != Type::Kind::Function || actual.nullable) {
    AddDiagnostic(expression.location, "array callback must be a non-null function");
    return InvalidType();
  }
  const auto& signature = function_types_.at(actual.name);
  if (signature.parameters.size() > expected.parameters.size()) {
    AddDiagnostic(expression.location, "array callback declares too many parameters");
    return InvalidType();
  }
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    if (!CanAssign(signature.parameters.at(index).type, expected.parameters.at(index).type)) {
      AddDiagnostic(expression.location, "array callback parameter type does not match");
    }
  }
  return actual;
}

TypeChecker::Type TypeChecker::CheckArrayAlgorithmResult(ArrayAlgorithm algorithm,
                                                         const Type& result,
                                                         const FunctionInfo& method,
                                                         SourceLocation location) {
  if (algorithm == ArrayAlgorithm::Map) {
    if (!IsValid(result) || result.kind == Type::Kind::Void || result.kind == Type::Kind::Null) {
      AddDiagnostic(location, "map callback must return a value with a concrete type");
      return InvalidType();
    }
    return ArrayType(result);
  }
  if (algorithm == ArrayAlgorithm::Sort) {
    if (result.nullable || (result.kind != Type::Kind::Int && result.kind != Type::Kind::Double)) {
      AddDiagnostic(location, "sort comparator must return int or double");
    }
    return method.return_type;
  }
  if (algorithm == ArrayAlgorithm::ForEach) {
    return method.return_type;
  }
  const Type boolean{.kind = Type::Kind::Bool, .name = "bool"};
  const auto& expected = algorithm == ArrayAlgorithm::Reduce ? method.return_type : boolean;
  if (!CanAssign(expected, result)) {
    AddDiagnostic(location, "array callback return type does not match");
  }
  return method.return_type;
}

TypeChecker::Type TypeChecker::CheckArrayAlgorithm(const Expression& expression,
                                                   const Type& receiver,
                                                   ArrayAlgorithm algorithm) {
  const auto element = array_elements_.at(receiver.name);
  if (algorithm == ArrayAlgorithm::Sort) {
    static_cast<void>(ArrayType({.kind = Type::Kind::Int, .name = "int"}));
  }
  auto accumulator = algorithm == ArrayAlgorithm::Map ? InvalidType() : element;
  std::vector<std::optional<Type>> checked(expression.arguments.size());
  std::optional<std::size_t> callback_index;
  const std::string_view callback_name =
      algorithm == ArrayAlgorithm::Sort ? "compareFn" : "callbackFn";
  // 初值先提供累加器类型，但运行时仍按源码顺序求值全部实参。
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    const auto& argument = expression.arguments.at(index);
    const bool positional = argument.name.empty();
    if (argument.name == callback_name || (positional && index == 0)) {
      callback_index = index;
    } else if (algorithm == ArrayAlgorithm::Reduce &&
               (argument.name == "initialValue" || (positional && index == 1))) {
      accumulator = CheckExpression(*argument.value, nullptr);
      checked.at(index) = accumulator;
    }
  }
  auto method = ArrayAlgorithmSignature(algorithm, element, accumulator);
  if (callback_index) {
    const auto& callback = *expression.arguments.at(*callback_index).value;
    auto expected = ArrayCallbackSignature(algorithm, element, accumulator);
    if (algorithm == ArrayAlgorithm::ForEach || algorithm == ArrayAlgorithm::Sort) {
      expected.return_type = InvalidType();
    }
    const auto actual = CheckArrayCallback(callback, expected);
    if (!IsValid(actual)) {
      return InvalidType();
    }
    const auto result = function_types_.at(actual.name).return_type;
    method.return_type = CheckArrayAlgorithmResult(algorithm, result, method, callback.location);
    method.parameters.front().type = actual;
    checked.at(*callback_index) = actual;
  } else if (algorithm == ArrayAlgorithm::Sort && !CanFormatString(element)) {
    AddDiagnostic(expression.location, "sort requires a comparator for this element type");
  }
  array_algorithms_.insert_or_assign(&expression, algorithm);
  return CheckFunctionCall(method, expression, checked);
}

TypeChecker::Type TypeChecker::CheckArrayMethod(const Expression& expression,
                                                const Type& receiver) {
  const auto& name = expression.callee->text;
  const auto element = array_elements_.at(receiver.name);
  if (name == "join") {
    if (!CanFormatString(element)) {
      AddDiagnostic(expression.location,
                    "join requires string, int, double, or bool elements (possibly nullable)");
      return InvalidType();
    }
    return CheckBuiltinCall(expression, Builtin::ArrayJoin);
  }
  if (const auto algorithm = FindArrayAlgorithm(name)) {
    return CheckArrayAlgorithm(expression, receiver, *algorithm);
  }
  for (const auto& [method, builtin] : ArrayMethods()) {
    if (method == name) {
      auto signature = ArrayMethodSignature(builtin, element);
      if (builtin == Builtin::ArrayPop || builtin == Builtin::ArrayShift) {
        signature.return_type = NullableType(element);
      }
      builtin_calls_.emplace(&expression, builtin);
      return CheckFunctionCall(signature, expression);
    }
  }
  AddDiagnostic(expression.location, "unknown array method '" + name + "'");
  return InvalidType();
}

}  // namespace vela::frontend
