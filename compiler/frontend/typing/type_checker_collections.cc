#include "compiler/frontend/typing/type_checker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/builtins/builtins.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_inference.h"
#include "compiler/frontend/typing/type_name.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

TypeChecker::Type TypeChecker::NullableType(Type type) {
  type = MakeNullableType(type);
  types_.try_emplace(type.name, type);
  return type;
}

TypeChecker::Type TypeChecker::ResolveGenericType(const TypeName& source) {
  Type::Kind kind = Type::Kind::Invalid;
  std::size_t count = 2;
  if (source.name == kFutureTypeName) {
    kind = Type::Kind::Future;
    count = 1;
  } else if (source.name == kCompleterTypeName) {
    kind = Type::Kind::Completer;
    count = 1;
  } else if (source.name == kSetTypeName) {
    kind = Type::Kind::Set;
    count = 1;
  } else if (source.name == kMapTypeName) {
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
    name += index == 0 ? "" : ", ";
    name += argument.name;
    if (kind == Type::Kind::Set || kind == Type::Kind::Map) {
      static_cast<void>(ArrayType(argument));
    }
    arguments.push_back(argument);
  }
  name += '>';
  Type type{
      .kind = kind,
      .name = InternTypeName(name),
      .is_abstract =
          std::ranges::any_of(arguments, [](const auto& argument) { return argument.is_abstract; }),
  };
  types_.try_emplace(name, type);
  generic_arguments_.try_emplace(name, std::move(arguments));
  return type;
}

TypeChecker::Type TypeChecker::CheckCollectionConstruction(const Expression& expression,
                                                           const Type* expected) {
  const auto& callee = *expression.callee;
  const bool set =
      callee.text == kSetTypeName ||
      (callee.text != kMapTypeName && expected != nullptr && expected->kind == Type::Kind::Set);
  const std::array parameters{IntrinsicTypeParameter("$Key"), IntrinsicTypeParameter("$Value")};
  const auto type = InferConstruction(expression, expected, set ? kSetTypeName : kMapTypeName,
                                      std::span(parameters).first(set ? 1 : 2));
  collection_constructions_.insert(&expression);
  return type;
}

TypeChecker::Type TypeChecker::CheckMapLiteral(const Expression& expression, const Type* expected) {
  Type key_type;
  Type value_type;
  if (expected != nullptr && expected->kind == Type::Kind::Map) {
    const auto map = NonNullable(*expected);
    const auto& arguments = generic_arguments_.at(map.name);
    key_type = arguments.front();
    value_type = arguments.back();
  }
  const auto check = [this](const Expression& item, Type& target, std::string_view role) {
    const auto actual = CheckExpression(item, IsValid(target) ? &target : nullptr);
    if (!IsValid(actual)) {
      return;
    }
    const auto prefix = "map " + std::string(role);
    if (actual.kind == Type::Kind::Void) {
      AddDiagnostic(item.location, prefix + " must be a value");
    } else if (actual.kind == Type::Kind::Null && !IsValid(target)) {
      AddDiagnostic(item.location, "null " + prefix + " requires an expected nullable type");
    } else if (!IsValid(target)) {
      target = actual;
    } else if (!CanAssign(target, actual)) {
      AddDiagnostic(item.location, prefix + " type does not match '" + TypeNameFor(target) + "'");
    } else {
      RecordConversion(item, target, actual);
    }
  };
  for (const auto& entry : expression.entries) {
    if (entry.key) {
      check(*entry.key, key_type, "key");
    }
    if (entry.value) {
      check(*entry.value, value_type, "value");
    }
    if (!entry.key || !entry.value) {
      AddDiagnostic(expression.location, "map entries require a key and a value");
    }
  }
  if (!IsValid(key_type) || !IsValid(value_type)) {
    if (expression.entries.empty()) {
      AddDiagnostic(expression.location, "empty map requires an expected map type");
    }
    return InvalidType();
  }
  return ResolveGenericType({
      .name = std::string(kMapTypeName),
      .location = expression.location,
      .kind = TypeName::Kind::Generic,
      .arguments = {DescribeType(key_type), DescribeType(value_type)},
      .parameter = {},
  });
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
  signature.return_type = {.kind = Type::Kind::Void, .name = kVoidTypeName};
  const auto operation = receiver.kind == Type::Kind::Set ? FindSetOperation(name) : std::nullopt;
  if (operation) {
    signature.return_type = receiver;
    signature.AddParameter("other", receiver);
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
    signature.return_type = {.kind = Type::Kind::Bool, .name = kBoolTypeName};
    signature.AddParameter(receiver.kind == Type::Kind::Set ? "value" : "key", arguments.front());
  } else if (name == "clear") {
    builtin = Builtin::CollectionClear;
  } else if (name == "get" && receiver.kind == Type::Kind::Map) {
    builtin = Builtin::MapGet;
    signature.AddParameter("key", arguments.front());
    signature.return_type = MakeNullableType(arguments.back());
  } else if (name == "set" && receiver.kind == Type::Kind::Map) {
    builtin = Builtin::MapSet;
    signature.AddParameter("key", arguments.front());
    signature.AddParameter("value", arguments.back());
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
