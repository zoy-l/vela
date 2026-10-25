#include "compiler/frontend/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"

namespace vela::frontend {

auto TypeChecker::CheckArrayAlgorithm(const Expression& expression,
                                      const Type& receiver,
                                      ArrayAlgorithm algorithm) -> Type {
  const auto element = array_elements_.at(receiver.name);
  auto accumulator = element;
  std::vector<Type> checked;
  if (algorithm == ArrayAlgorithm::Reduce) {
    checked.reserve(expression.arguments.size());
    for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
      const auto& argument = expression.arguments.at(index);
      auto type = CheckExpression(*argument.value, nullptr);
      if (argument.name == "initial" || (argument.name.empty() && index == 0)) {
        accumulator = type;
      }
      checked.push_back(std::move(type));
    }
  }
  static_cast<void>(FunctionType(ArrayCallbackSignature(algorithm, element, accumulator)));
  array_algorithms_.insert_or_assign(&expression, algorithm);
  return CheckFunctionCall(ArrayAlgorithmSignature(algorithm, element, accumulator), expression,
                           checked);
}

auto TypeChecker::CheckArrayMethod(const Expression& expression, const Type& receiver) -> Type {
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
  FunctionInfo signature;
  signature.return_type = {.kind = Type::Kind::Void, .name = "void"};
  const auto parameter = [&](std::string key, Type type) -> void {
    signature.parameter_indices.emplace(key,
                                        static_cast<std::uint32_t>(signature.parameters.size()));
    signature.parameters.push_back({.name = std::move(key), .type = std::move(type)});
  };
  Builtin builtin = Builtin::ArrayReverse;
  if (name == "copy") {
    builtin = Builtin::ArrayCopy;
    signature.return_type = receiver;
  } else if (name == "slice") {
    builtin = Builtin::ArraySlice;
    signature.return_type = receiver;
    parameter("start", {.kind = Type::Kind::Int, .name = "int"});
    parameter("end", {.kind = Type::Kind::Int, .name = "int"});
  } else if (name == "fill") {
    builtin = Builtin::ArrayFill;
    parameter("value", array_elements_.at(receiver.name));
  } else if (name != "reverse") {
    AddDiagnostic(expression.location, "unknown array method '" + name + "'");
    return InvalidType();
  }
  builtin_calls_.emplace(&expression, builtin);
  return CheckFunctionCall(signature, expression);
}

}  // namespace vela::frontend
