#include "compiler/frontend/type_checker.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"

namespace vela::frontend {

TypeChecker::Type TypeChecker::NullableType(Type type) {
  type = MakeNullableType(std::move(type));
  types_.try_emplace(type.name, type);
  return type;
}

TypeChecker::Type TypeChecker::ResolveGenericType(const TypeName& source) {
  Type::Kind kind = Type::Kind::Invalid;
  std::size_t count = 2;
  if (source.name == "Future") {
    kind = Type::Kind::Future;
    count = 1;
  } else if (source.name == "Completer") {
    kind = Type::Kind::Completer;
    count = 1;
  } else if (source.name == "Set") {
    kind = Type::Kind::Set;
    count = 1;
  } else if (source.name == "Map") {
    kind = Type::Kind::Map;
  }
  if (kind == Type::Kind::Invalid || source.arguments.size() != count) {
    AddDiagnostic(source.location,
                  "invalid generic type or type argument count for '" + source.name + "'");
    return InvalidType();
  }
  std::vector<Type> arguments;
  std::string name = source.name + '<';
  for (std::size_t index = 0; index < count; ++index) {
    auto argument = ResolveType(source.arguments.at(index),
                                kind == Type::Kind::Future || kind == Type::Kind::Completer);
    if (!IsValid(argument)) {
      return InvalidType();
    }
    if ((kind == Type::Kind::Future || kind == Type::Kind::Completer) &&
        argument.kind == Type::Kind::Future) {
      AddDiagnostic(source.location, "Future payload types must be flattened");
      return InvalidType();
    }
    name += (index == 0 ? "" : ", ") + argument.name;
    if (kind == Type::Kind::Set || kind == Type::Kind::Map) {
      static_cast<void>(ArrayType(argument));
    }
    arguments.push_back(std::move(argument));
  }
  name += '>';
  Type type{.kind = kind, .name = name};
  types_.try_emplace(name, type);
  generic_arguments_.try_emplace(name, std::move(arguments));
  return type;
}

TypeChecker::Type TypeChecker::CheckCollectionConstruction(const Expression& expression,
                                                           const Type* expected) {
  const auto& callee = *expression.callee;
  const bool set = callee.text == "Set" || (callee.text != "Map" && expected != nullptr &&
                                            expected->kind == Type::Kind::Set);
  const std::array<std::string, 2> parameters{"$Key", "$Value"};
  const auto type = InferConstruction(expression, expected, set ? "Set" : "Map",
                                      std::span(parameters).first(set ? 1 : 2));
  collection_constructions_.insert(&expression);
  return type;
}

std::optional<TypeChecker::BuiltinMethod> TypeChecker::ResolveCollectionMethod(
    const Type& receiver,
    std::string_view name) {
  auto method = ResolveCollectionBuiltin(receiver, generic_arguments_.at(receiver.name), name);
  if (!method) {
    return std::nullopt;
  }
  const auto& result = method->signature.return_type;
  if (result.kind == Type::Kind::Array) {
    static_cast<void>(
        ArrayType(generic_arguments_.at(receiver.name)
                      .at(name == "keys" || receiver.kind == Type::Kind::Set ? 0 : 1)));
  } else if (result.nullable) {
    types_.try_emplace(result.name, result);
  }
  return BuiltinMethod{.operation = method->operation, .signature = std::move(method->signature)};
}

std::optional<CollectionMethod> ResolveCollectionBuiltin(const TypeDescriptor& receiver,
                                                         std::span<const TypeDescriptor> arguments,
                                                         std::string_view name) {
  using Type = TypeDescriptor;
  FunctionDescriptor signature;
  signature.return_type = {.kind = Type::Kind::Void, .name = "void"};
  const auto add = [&](std::string parameter, Type type) -> void {
    signature.parameter_indices.emplace(parameter,
                                        static_cast<std::uint32_t>(signature.parameters.size()));
    signature.parameters.push_back({.name = std::move(parameter), .type = std::move(type)});
  };
  const auto operation = receiver.kind == Type::Kind::Set ? FindSetOperation(name) : std::nullopt;
  if (operation) {
    signature.return_type = receiver;
    add("other", receiver);
    return CollectionMethod{.operation = *operation, .signature = std::move(signature)};
  }
  Builtin builtin = Builtin::CollectionSize;
  if (name == "has" || name == "delete" || (name == "add" && receiver.kind == Type::Kind::Set)) {
    if (name == "has") {
      builtin = Builtin::CollectionHas;
    } else if (name == "delete") {
      builtin = Builtin::CollectionDelete;
    } else {
      builtin = Builtin::SetAdd;
    }
    signature.return_type = {.kind = Type::Kind::Bool, .name = "bool"};
    add(receiver.kind == Type::Kind::Set ? "value" : "key", arguments.front());
  } else if (name == "clear") {
    builtin = Builtin::CollectionClear;
  } else if (name == "get" && receiver.kind == Type::Kind::Map) {
    builtin = Builtin::MapGet;
    add("key", arguments.front());
    signature.return_type = MakeNullableType(arguments.back());
  } else if (name == "set" && receiver.kind == Type::Kind::Map) {
    builtin = Builtin::MapSet;
    add("key", arguments.front());
    add("value", arguments.back());
  } else if (name == "values" || (name == "keys" && receiver.kind == Type::Kind::Map)) {
    const bool keys = name == "keys" || receiver.kind == Type::Kind::Set;
    builtin = keys ? Builtin::CollectionKeys : Builtin::CollectionValues;
    signature.return_type = MakeArrayType(keys ? arguments.front() : arguments.back());
  } else {
    return std::nullopt;
  }
  return CollectionMethod{.operation = builtin, .signature = std::move(signature)};
}

TypeChecker::Type TypeChecker::CheckCollectionMethod(const Expression& expression,
                                                     const Type& receiver) {
  const auto method = ResolveCollectionMethod(receiver, expression.callee->text);
  if (!method) {
    AddDiagnostic(expression.location,
                  "unknown collection method '" + expression.callee->text + "'");
    return InvalidType();
  }
  builtin_calls_.insert_or_assign(&expression, method->operation);
  return CheckFunctionCall(method->signature, expression);
}

}  // namespace vela::frontend
