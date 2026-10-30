#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

TypeChecker::Type TypeChecker::ResolveType(const TypeName& type_name, bool allow_void) {
  if (type_name.kind == TypeName::Kind::Generic &&
      specializer_->ClassTemplate(type_name.name) != nullptr) {
    return ResolveClassInstantiation(type_name);
  }
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
    if (element.nullable || element.kind == Type::Kind::Dynamic) {
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

TypeDescriptor MakeNullableType(TypeDescriptor type) {
  if (!type.nullable && type.kind != TypeDescriptor::Kind::Dynamic) {
    type.nullable = true;
    type.name =
        type.kind == TypeDescriptor::Kind::Function ? "(" + type.name + ")?" : type.name + '?';
  }
  return type;
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
  if ((target.kind == Type::Kind::Dynamic || value.kind == Type::Kind::Dynamic) &&
      IsValid(target) && IsValid(value) && target.kind != Type::Kind::Void &&
      value.kind != Type::Kind::Void) {
    return true;
  }
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
    const auto& payload = generic_arguments_.at(NonNullable(value).name).front();
    const auto& expected = generic_arguments_.at(NonNullable(target).name).front();
    return (payload.kind != Type::Kind::Dynamic || expected.kind == Type::Kind::Dynamic) &&
           CanAssign(expected, payload);
  }
  return false;
}

void TypeChecker::RecordConversion(const Expression& expression,
                                   const Type& target,
                                   const Type& actual) {
  if ((actual.kind == Type::Kind::Dynamic && target.kind != Type::Kind::Dynamic &&
       IsValid(target) && target.kind != Type::Kind::Void) ||
      (target.kind == Type::Kind::Dynamic && actual.kind == Type::Kind::Enum)) {
    const auto inserted = checked_conversions_.insert_or_assign(&expression, target).second;
    if (inserted && actual.kind == Type::Kind::Dynamic) {
      DiagnoseDynamicConversion(expression, target, actual);
    }
  }
}

void TypeChecker::DiagnoseDynamicConversion(const Expression& expression,
                                            const Type& target,
                                            const Type& actual) {
  if (actual.kind != Type::Kind::Dynamic || target.kind == Type::Kind::Dynamic ||
      target.kind == Type::Kind::Void || !IsValid(target)) {
    return;
  }
  const auto location = expression.location;
  if (!dynamic_diagnostics_.emplace(location.source_id, location.line, location.column, target.name)
           .second) {
    return;
  }
  AddDiagnostic(expression.location, "implicit conversion from dynamic to '" + target.name +
                                         "' may fail at runtime; use 'as' or narrow with 'is'");
  auto& diagnostic = diagnostics_.back();
  diagnostic.severity = options_.implicit_dynamic_conversion;
  diagnostic.code = DiagnosticCode::ImplicitDynamicConversion;
}

void TypeChecker::CheckBooleanOperand(const Expression& expression,
                                      const Type& type,
                                      std::string_view message) {
  const Type boolean{.kind = Type::Kind::Bool, .name = "bool"};
  RecordConversion(expression, boolean, type);
  if (!CanAssign(boolean, type)) {
    AddDiagnostic(expression.location, std::string(message));
  }
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
