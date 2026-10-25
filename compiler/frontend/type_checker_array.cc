#include "compiler/frontend/type_checker.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"

namespace vela::frontend {

TypeChecker::Type TypeChecker::CheckArrayAlgorithm(const Expression& expression,
                                                   const Type& receiver,
                                                   ArrayAlgorithm algorithm) {
  if (algorithm == ArrayAlgorithm::Map) {
    return CheckArrayMap(expression, receiver);
  }
  const auto element = array_elements_.at(receiver.name);
  auto accumulator = element;
  std::vector<std::optional<Type>> checked;
  if (algorithm == ArrayAlgorithm::Reduce) {
    checked.resize(expression.arguments.size());
    for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
      const auto& argument = expression.arguments.at(index);
      const bool initial = argument.name == "initial" || (argument.name.empty() && index == 0);
      // lambda 的参数依赖 initial；先综合独立实参，再由统一调用检查传入期望签名。
      if (!initial && argument.value->kind == Expression::Kind::Lambda) {
        continue;
      }
      auto type = CheckExpression(*argument.value, nullptr);
      if (initial) {
        accumulator = type;
      }
      checked.at(index) = std::move(type);
    }
  }
  static_cast<void>(FunctionType(ArrayCallbackSignature(algorithm, element, accumulator)));
  array_algorithms_.insert_or_assign(&expression, algorithm);
  return CheckFunctionCall(ArrayAlgorithmSignature(algorithm, element, accumulator), expression,
                           checked);
}

TypeChecker::Type TypeChecker::CheckArrayMap(const Expression& expression, const Type& receiver) {
  if (expression.arguments.size() != 1) {
    AddDiagnostic(expression.location, "map expects one callback");
    return InvalidType();
  }
  const auto element = array_elements_.at(receiver.name);
  const auto context =
      FunctionType(ArrayCallbackSignature(ArrayAlgorithm::Map, element, InvalidType()));
  const auto actual = CheckExpression(*expression.arguments.front().value, &context);
  const auto result = actual.kind == Type::Kind::Function && !actual.nullable
                          ? function_types_.at(actual.name).return_type
                          : InvalidType();
  // 仅在检查回调时提供参数约束，未定返回类型不进入最终语义模型。
  function_types_.erase(context.name);
  types_.erase(context.name);
  if (actual.kind != Type::Kind::Function || actual.nullable) {
    AddDiagnostic(expression.location, "map requires a non-null function callback");
    return InvalidType();
  }
  if (!IsValid(result) || result.kind == Type::Kind::Void || result.kind == Type::Kind::Null) {
    AddDiagnostic(expression.location, "map callback must return a value with a concrete type");
    return InvalidType();
  }
  static_cast<void>(ArrayType(result));
  array_algorithms_.insert_or_assign(&expression, ArrayAlgorithm::Map);
  const std::array<std::optional<Type>, 1> checked = {actual};
  return CheckFunctionCall(ArrayAlgorithmSignature(ArrayAlgorithm::Map, element, result),
                           expression, checked);
}

TypeChecker::Type TypeChecker::CheckArrayMethod(const Expression& expression,
                                                const Type& receiver) {
  const auto& name = expression.callee->text;
  if (name == "join") {
    if (!CanFormatString(array_elements_.at(receiver.name))) {
      AddDiagnostic(expression.location,
                    "join requires string, int, double, or bool elements (possibly nullable)");
      return InvalidType();
    }
    return CheckBuiltinCall(expression, Builtin::ArrayJoin);
  }
  if (const auto algorithm = FindArrayAlgorithm(name)) {
    return CheckArrayAlgorithm(expression, receiver, *algorithm);
  }
  const auto element = array_elements_.at(receiver.name);
  for (const auto& [method, builtin] : ArrayMethods()) {
    if (method == name) {
      auto signature = ArrayMethodSignature(builtin, receiver, element);
      if (builtin == Builtin::ArrayPop) {
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
