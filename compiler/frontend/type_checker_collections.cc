#include "compiler/frontend/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"

namespace vela::frontend {

auto TypeChecker::NullableType(Type type) -> Type {
  if (!type.nullable) {
    type.nullable = true;
    type.name = type.kind == Type::Kind::Function ? "(" + type.name + ")?" : type.name + '?';
    types_.try_emplace(type.name, type);
  }
  return type;
}

auto TypeChecker::ResolveGenericType(const TypeName& source) -> Type {
  Type::Kind kind = Type::Kind::Invalid;
  std::size_t count = 2;
  if (source.name == "Expected") {
    kind = Type::Kind::Expected;
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
    auto argument =
        ResolveType(source.arguments.at(index), kind == Type::Kind::Expected && index == 0);
    if (!IsValid(argument)) {
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

auto TypeChecker::CheckCollectionConstruction(const Expression& expression, const Type* expected)
    -> Type {
  const auto& callee = *expression.callee;
  Type type;
  if (!callee.type_arguments.empty()) {
    type = ResolveGenericType({
        .name = callee.text,
        .location = callee.location,
        .kind = TypeName::Kind::Generic,
        .arguments = callee.type_arguments,
    });
  } else if (expected != nullptr &&
             ((callee.text == "Set" && expected->kind == Type::Kind::Set) ||
              (callee.text == "Map" && expected->kind == Type::Kind::Map) ||
              (callee.text != "Set" && callee.text != "Map" &&
               (expected->kind == Type::Kind::Set || expected->kind == Type::Kind::Map)))) {
    type = NonNullable(*expected);
  } else {
    AddDiagnostic(expression.location,
                  "Set/Map construction requires explicit or contextual type arguments");
  }
  if (!expression.arguments.empty()) {
    AddDiagnostic(expression.location, "Set/Map constructors do not accept arguments");
  }
  collection_constructions_.insert(&expression);
  return type;
}

auto TypeChecker::CheckCollectionMethod(const Expression& expression, const Type& receiver)
    -> Type {
  const auto arguments = generic_arguments_.at(receiver.name);
  const auto& name = expression.callee->text;
  FunctionInfo signature;
  signature.return_type = {.kind = Type::Kind::Void, .name = "void"};
  const auto add = [&](std::string parameter, Type type) -> void {
    signature.parameter_indices.emplace(parameter,
                                        static_cast<std::uint32_t>(signature.parameters.size()));
    signature.parameters.push_back({.name = std::move(parameter), .type = std::move(type)});
  };
  Builtin builtin = Builtin::CollectionLength;
  if (name == "has" || name == "remove" || (name == "add" && receiver.kind == Type::Kind::Set)) {
    if (name == "has") {
      builtin = Builtin::CollectionHas;
    } else if (name == "remove") {
      builtin = Builtin::CollectionRemove;
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
    signature.return_type = NullableType(arguments.back());
  } else if (name == "set" && receiver.kind == Type::Kind::Map) {
    builtin = Builtin::MapSet;
    add("key", arguments.front());
    add("value", arguments.back());
  } else if (name == "values" || (name == "keys" && receiver.kind == Type::Kind::Map)) {
    const bool keys = name == "keys" || receiver.kind == Type::Kind::Set;
    builtin = keys ? Builtin::CollectionKeys : Builtin::CollectionValues;
    signature.return_type = ArrayType(keys ? arguments.front() : arguments.back());
  } else {
    AddDiagnostic(expression.location, "unknown collection method '" + name + "'");
    return InvalidType();
  }
  builtin_calls_.insert_or_assign(&expression, builtin);
  return CheckFunctionCall(signature, expression);
}

}  // namespace vela::frontend
