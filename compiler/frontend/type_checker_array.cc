#include "base/array_algorithm.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"

namespace vela::frontend {

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
                                                   ArrayAlgorithm algorithm,
                                                   const Type* expected) {
  const auto element = array_elements_.at(receiver.name);
  if (algorithm == ArrayAlgorithm::Sort) {
    static_cast<void>(ArrayType({.kind = Type::Kind::Int, .name = "int"}));
  }
  auto method = ArrayAlgorithmSignature(algorithm, element, element);
  auto callback = ArrayCallbackSignature(algorithm, element, element);
  static_cast<void>(ArrayType(element));
  InferenceSignature source;
  source.optional_parameters = method.optional_parameters;
  auto callback_type = DescribeType(FunctionType(callback));
  const bool generic_result =
      algorithm == ArrayAlgorithm::Map || algorithm == ArrayAlgorithm::Reduce ||
      algorithm == ArrayAlgorithm::ForEach || algorithm == ArrayAlgorithm::Sort;
  const TypeName variable{.name = "$Result", .location = expression.location, .arguments = {}};
  if (generic_result && !expression.arguments.empty()) {
    source.type_parameters.push_back(variable.name);
    callback_type.arguments.back() = variable;
    if (algorithm == ArrayAlgorithm::Reduce) {
      callback_type.arguments.front() = variable;
    }
  }
  source.return_type = DescribeType(method.return_type);
  if (algorithm == ArrayAlgorithm::Map) {
    source.return_type = {
        .name = {},
        .location = expression.location,
        .kind = TypeName::Kind::Array,
        .arguments = {variable},
    };
  } else if (algorithm == ArrayAlgorithm::Reduce) {
    source.return_type = variable;
  }
  source.parameters.push_back({
      .type = std::move(callback_type),
      .name = method.parameters.front().name,
      .location = expression.location,
  });
  if (algorithm == ArrayAlgorithm::Reduce) {
    source.parameters.push_back(
        {.type = variable, .name = "initialValue", .location = expression.location});
  }
  auto inferred = InferInvocation(source, expression, expected, {}, CallbackArity::Prefix);
  if (!inferred) {
    return InvalidType();
  }
  method.return_type = inferred->signature.return_type;
  for (std::size_t index = 0; index < inferred->arguments.size(); ++index) {
    const auto& argument = expression.arguments.at(index);
    if ((argument.name.empty() && index == 0) || argument.name == source.parameters.front().name) {
      const auto actual = *inferred->arguments.at(index);
      if (actual.kind != Type::Kind::Function) {
        return InvalidType();
      }
      CheckCallbackContract(*argument.value, actual,
                            function_types_.at(inferred->signature.parameters.front().type.name),
                            CallbackArity::Prefix, CallbackParameters::Assignable);
      const auto result = function_types_.at(actual.name).return_type;
      if (algorithm == ArrayAlgorithm::Reduce) {
        DiagnoseDynamicConversion(*argument.value, method.return_type, result);
      }
      method.return_type = CheckArrayAlgorithmResult(algorithm, result, method, argument.location);
      inferred->signature.parameters.front().type = actual;
    }
  }
  if (algorithm == ArrayAlgorithm::Sort && expression.arguments.empty() &&
      !CanFormatString(element)) {
    AddDiagnostic(expression.location, "sort requires a comparator for this element type");
  }
  inferred->signature.return_type = method.return_type;
  array_algorithms_.insert_or_assign(&expression, algorithm);
  return CheckFunctionCall(inferred->signature, expression, inferred->arguments);
}

TypeChecker::Type TypeChecker::CheckArrayMethod(const Expression& expression,
                                                const Type& receiver,
                                                const Type* expected) {
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
    return CheckArrayAlgorithm(expression, receiver, *algorithm, expected);
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
