#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_name.h"
#include "compiler/frontend/type_names.h"

namespace vela::frontend {

void FunctionDescriptor::AddParameter(std::string name,
                                      TypeDescriptor type,
                                      ParameterPresence presence) {
  if (presence == ParameterPresence::Optional) {
    optional_parameters.insert(name);
  }
  parameter_indices.emplace(name, static_cast<std::uint32_t>(parameters.size()));
  parameters.push_back({.name = std::move(name), .type = type});
}

TypeChecker::Type TypeChecker::ResolveType(const TypeName& type_name, bool allow_void) {
  if (type_name.kind == TypeName::Kind::Parameter) {
    return type_parameters_.at(type_name.parameter).type;
  }
  if (specializer_->AliasTemplate(type_name.name) != nullptr) {
    const auto expanded = specializer_->ExpandAlias(type_name);
    for (auto& diagnostic : specializer_->TakeDiagnostics()) {
      diagnostics_.push_back(std::move(diagnostic));
    }
    if (expanded.kind != type_name.kind || expanded.name != type_name.name) {
      return ResolveType(expanded, allow_void);
    }
    return InvalidType();
  }
  if (type_name.kind == TypeName::Kind::Generic &&
      (specializer_->ClassTemplate(type_name.name) != nullptr ||
       specializer_->InterfaceTemplate(type_name.name) != nullptr)) {
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
    return NullableType(element);
  }
  if (type_name.kind == TypeName::Kind::Function) {
    return ResolveFunctionType(type_name);
  }
  const auto result = ResolveNamedType(type_name);
  if (!allow_void && result.kind == Type::Kind::Void) {
    AddDiagnostic(type_name.location, "void is not a value type");
    return InvalidType();
  }
  return result;
}

TypeChecker::Type TypeChecker::ResolveNamedType(const TypeName& type_name) {
  const auto found = types_.find(type_name.name);
  if (found != types_.end()) {
    return found->second;
  }
  if (const auto alias = aliases_.find(type_name.name); alias != aliases_.end()) {
    if (!resolving_aliases_.insert(type_name.name).second) {
      AddDiagnostic(type_name.location, "cyclic type alias '" + type_name.name + "'");
      return InvalidType();
    }
    const auto result = ResolveType(alias->second->target, true);
    resolving_aliases_.erase(type_name.name);
    types_.try_emplace(type_name.name, result);
    return result;
  }
  AddDiagnostic(type_name.location, "unknown type '" + type_name.name + "'");
  return InvalidType();
}

TypeChecker::Type TypeChecker::ResolveFunctionType(const TypeName& type_name) {
  FunctionInfo signature;
  for (std::size_t index = 0; index + 1 < type_name.arguments.size(); ++index) {
    signature.AddParameter(std::to_string(index),
                           ResolveType(type_name.arguments.at(index), false));
  }
  signature.return_type = ResolveType(type_name.arguments.back(), true);
  return FunctionType(signature);
}

TypeDescriptor MakeArrayType(const TypeDescriptor& element) {
  const std::string name = element.kind == TypeDescriptor::Kind::Function
                               ? "(" + std::string(element.name) + ")[]"
                               : std::string(element.name) + "[]";
  return {
      .kind = TypeDescriptor::Kind::Array,
      .name = InternTypeName(name),
      .is_abstract = element.is_abstract,
  };
}

TypeDescriptor MakeNullableType(TypeDescriptor type) {
  if (!type.nullable && type.kind != TypeDescriptor::Kind::Dynamic) {
    type.nullable = true;
    const std::string name = type.kind == TypeDescriptor::Kind::Function
                                 ? "(" + std::string(type.name) + ")?"
                                 : std::string(type.name) + '?';
    type.name = InternTypeName(name);
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
  if (type.kind == Type::Kind::Parameter) {
    type.known_non_null = true;
  }
  if (type.nullable) {
    type.nullable = false;
    type.name = type.name.substr(0, type.name.size() - 1);
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
  return IsSubtype(value, target);
}

bool TypeChecker::IsSubtype(const Type& subtype, const Type& supertype) const {
  if (!IsValid(subtype) || !IsValid(supertype)) {
    return false;
  }
  std::vector<Type> pending{subtype};
  absl::flat_hash_set<std::string_view> visited;
  while (!pending.empty()) {
    const auto current = pending.back();
    pending.pop_back();
    if (!visited.insert(current.name).second) {
      continue;
    }
    if (IsSameType(current, supertype)) {
      return true;
    }
    if (current.kind == Type::Kind::Null) {
      return supertype.nullable;
    }
    if (current.nullable && !supertype.nullable) {
      continue;
    }
    if (IsSameType(NonNullable(current), NonNullable(supertype))) {
      return true;
    }
    if (current.kind == Type::Kind::Dynamic || supertype.kind == Type::Kind::Dynamic) {
      continue;
    }
    if (current.kind == Type::Kind::Future && supertype.kind == Type::Kind::Future) {
      const auto& payload = generic_arguments_.at(NonNullable(current).name).front();
      const auto& expected = generic_arguments_.at(NonNullable(supertype).name).front();
      // Future 沿用载荷适配契约；dynamic 载荷不能证明其它载荷类型。
      return (payload.kind != Type::Kind::Dynamic || expected.kind == Type::Kind::Dynamic) &&
             CanAssign(expected, payload);
    }
    const auto parents = DirectSupertypes(current);
    pending.insert(pending.end(), parents.begin(), parents.end());
  }
  // 重复的参数转发或继承路径不能提供新的静态证明。
  return false;
}

std::vector<TypeChecker::Type> TypeChecker::DirectSupertypes(const Type& type) const {
  std::vector<Type> result;
  if (const auto* parameter = FindTypeParameter(type)) {
    if (parameter->bound) {
      result.push_back(*parameter->bound);
    }
  } else if (type.kind == Type::Kind::Class) {
    const auto& owner = classes_.at(NonNullable(type).name);
    if (owner.base_class) {
      result.push_back(types_.at(*owner.base_class));
    }
    for (const auto& interface : owner.interfaces) {
      result.push_back(types_.at(interface));
    }
  }
  for (auto& parent : result) {
    if (type.known_non_null) {
      parent = NonNullable(parent);
    } else if (type.nullable) {
      parent = MakeNullableType(parent);
    }
  }
  return result;
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
  AddDiagnostic(expression.location, "implicit conversion from dynamic to '" +
                                         std::string(target.name) +
                                         "' may fail at runtime; use 'as' or narrow with 'is'");
  auto& diagnostic = diagnostics_.back();
  diagnostic.severity = options_.implicit_dynamic_conversion;
  diagnostic.code = DiagnosticCode::ImplicitDynamicConversion;
}

void TypeChecker::CheckBooleanOperand(const Expression& expression,
                                      const Type& type,
                                      std::string_view message) {
  const Type boolean{.kind = Type::Kind::Bool, .name = kBoolTypeName};
  RecordConversion(expression, boolean, type);
  if (!CanAssign(boolean, type)) {
    DiagnoseOperandMismatch(expression, type, message);
  }
}

bool TypeChecker::IsNumeric(const Type& type) {
  return !type.nullable && (type.kind == Type::Kind::Int || type.kind == Type::Kind::Double);
}

bool TypeChecker::IsValid(const Type& type) {
  return type.kind != Type::Kind::Invalid;
}

std::string TypeChecker::TypeNameFor(const Type& type) const {
  if (type.name.empty()) {
    return "<invalid>";
  }
  return type.is_abstract ? DescribeType(type).ToString() : std::string(type.name);
}

}  // namespace vela::frontend
