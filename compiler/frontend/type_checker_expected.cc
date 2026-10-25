#include "compiler/frontend/type_checker.h"

#include <string>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

auto TypeChecker::CheckExpectedFactory(const Expression& expression, const Type* expected) -> Type {
  if (expected == nullptr || expected->kind != Type::Kind::Expected) {
    AddDiagnostic(expression.location, "ok/err requires an expected Expected<T, E> type");
    return InvalidType();
  }
  const auto type = NonNullable(*expected);
  const auto operation =
      expression.callee->text == "ok" ? ExpectedOperation::Ok : ExpectedOperation::Err;
  const auto& arguments = generic_arguments_.at(type.name);
  const auto payload = operation == ExpectedOperation::Ok ? arguments.front() : arguments.back();
  FunctionInfo signature;
  signature.return_type = type;
  if (payload.kind != Type::Kind::Void) {
    const std::string name = operation == ExpectedOperation::Ok ? "value" : "error";
    signature.parameter_indices.emplace(name, 0);
    signature.parameters.push_back({.name = name, .type = payload});
  }
  expected_calls_.insert_or_assign(&expression, operation);
  return CheckFunctionCall(signature, expression);
}

auto TypeChecker::CheckExpectedMethod(const Expression& expression, const Type& receiver) -> Type {
  const auto& name = expression.callee->text;
  const auto arguments = generic_arguments_.at(receiver.name);
  Type result;
  ExpectedOperation operation = ExpectedOperation::HasValue;
  if (name == "hasValue") {
    result = {.kind = Type::Kind::Bool, .name = "bool"};
  } else if (name == "value") {
    operation = ExpectedOperation::Value;
    result = arguments.front();
  } else if (name == "error") {
    operation = ExpectedOperation::Error;
    result = arguments.back();
  } else {
    AddDiagnostic(expression.location, "unknown Expected method '" + name + "'");
    return InvalidType();
  }
  if (!expression.arguments.empty()) {
    AddDiagnostic(expression.location, "Expected accessors do not accept arguments");
  }
  expected_calls_.insert_or_assign(&expression, operation);
  return result;
}

}  // namespace vela::frontend
