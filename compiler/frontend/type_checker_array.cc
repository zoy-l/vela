#include "compiler/frontend/type_checker.h"

#include <cstdint>
#include <string>
#include <utility>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"

namespace vela::frontend {

auto TypeChecker::CheckArrayMethod(const Expression& expression, const Type& receiver) -> Type {
  const auto& name = expression.callee->text;
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
