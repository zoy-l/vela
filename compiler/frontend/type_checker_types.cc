#include "compiler/frontend/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

TypeChecker::Type TypeChecker::ResolveType(const TypeName& type_name, bool allow_void) {
  if (type_name.kind == TypeName::Kind::Generic) {
    return ResolveGenericType(type_name);
  }
  if (type_name.kind == TypeName::Kind::Array || type_name.kind == TypeName::Kind::Nullable) {
    auto element = ResolveType(type_name.arguments.front(), false);
    if (!IsValid(element)) {
      return InvalidType();
    }
    if (type_name.kind == TypeName::Kind::Array) {
      return ArrayType(element);
    }
    if (element.nullable) {
      AddDiagnostic(type_name.location, "type is already nullable");
      return InvalidType();
    }
    return NullableType(std::move(element));
  }
  if (type_name.kind == TypeName::Kind::Function) {
    FunctionInfo signature;
    std::string name = "(";
    const auto count = type_name.arguments.size() - 1;
    for (std::size_t index = 0; index < count; ++index) {
      const auto parameter = ResolveType(type_name.arguments.at(index), false);
      const auto key = std::to_string(index);
      signature.parameter_indices.emplace(key,
                                          static_cast<std::uint32_t>(signature.parameters.size()));
      signature.parameters.push_back({.name = key, .type = parameter});
      name += (index == 0 ? "" : ", ") + parameter.name;
    }
    signature.return_type = ResolveType(type_name.arguments.back(), true);
    name += ") => " + signature.return_type.name;
    Type result{.kind = Type::Kind::Function, .name = name};
    function_types_.try_emplace(name, std::move(signature));
    types_.try_emplace(name, result);
    return result;
  }
  const auto found = types_.find(type_name.name);
  Type result;
  if (found != types_.end()) {
    result = found->second;
  } else if (const auto alias = aliases_.find(type_name.name); alias != aliases_.end()) {
    if (!resolving_aliases_.insert(type_name.name).second) {
      AddDiagnostic(type_name.location, "cyclic type alias '" + type_name.name + "'");
      return InvalidType();
    }
    result = ResolveType(alias->second->target, true);
    resolving_aliases_.erase(type_name.name);
    types_.try_emplace(type_name.name, result);
  } else {
    AddDiagnostic(type_name.location, "unknown type '" + type_name.name + "'");
    return InvalidType();
  }
  if (!allow_void && result.kind == Type::Kind::Void) {
    AddDiagnostic(type_name.location, "void is not a value type");
    return InvalidType();
  }
  return result;
}

TypeDescriptor MakeArrayType(const TypeDescriptor& element) {
  return {
      .kind = TypeDescriptor::Kind::Array,
      .name = (element.kind == TypeDescriptor::Kind::Function ? "(" + element.name + ")"
                                                              : element.name) +
              "[]",
  };
}

TypeChecker::Type TypeChecker::ArrayType(const Type& element) {
  const auto result = MakeArrayType(element);
  array_elements_.try_emplace(result.name, element);
  types_.try_emplace(result.name, result);
  return result;
}

bool TypeChecker::IsSameType(const Type& left, const Type& right) {
  return left.kind == right.kind && left.name == right.name && left.nullable == right.nullable;
}

TypeChecker::Type TypeChecker::NonNullable(Type type) {
  if (type.nullable) {
    type.nullable = false;
    type.name.pop_back();
    if (type.kind == Type::Kind::Function) {
      type.name = type.name.substr(1, type.name.size() - 2);
    }
  }
  return type;
}

bool TypeChecker::CanAssign(const Type& target, const Type& value) const {
  if (IsSameType(target, value) || (target.nullable && (value.kind == Type::Kind::Null ||
                                                        IsSameType(NonNullable(target), value)))) {
    return true;
  }
  if (target.kind == Type::Kind::Interface && value.kind == Type::Kind::Class &&
      (target.nullable || !value.nullable)) {
    const auto found = classes_.find(NonNullable(value).name);
    return found != classes_.end() && found->second.interfaces.contains(NonNullable(target).name);
  }
  if (target.kind == Type::Kind::Future && value.kind == Type::Kind::Future &&
      (target.nullable || !value.nullable)) {
    return CanAssign(generic_arguments_.at(NonNullable(target).name).front(),
                     generic_arguments_.at(NonNullable(value).name).front());
  }
  return false;
}

bool TypeChecker::IsNumeric(const Type& type) {
  return !type.nullable && (type.kind == Type::Kind::Int || type.kind == Type::Kind::Double);
}

bool TypeChecker::IsValid(const Type& type) {
  return type.kind != Type::Kind::Invalid;
}

std::string TypeChecker::TypeNameFor(const Type& type) {
  return type.name.empty() ? "<invalid>" : type.name;
}

}  // namespace vela::frontend
