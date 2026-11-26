#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/declaration.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/names.h"

namespace vela::frontend {

bool TypeChecker::MatchesImplementationSignature(const FunctionInfo& implementation,
                                                 const FunctionInfo& signature) {
  if (implementation.visibility != Visibility::Public ||
      implementation.storage != MemberStorage::Instance ||
      implementation.is_getter != signature.is_getter ||
      implementation.is_setter != signature.is_setter ||
      !IsSameType(implementation.return_type, signature.return_type) ||
      implementation.parameters.size() != signature.parameters.size()) {
    return false;
  }
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& actual = implementation.parameters[index];
    const auto& expected = signature.parameters[index];
    if (!IsSameType(actual.type, expected.type) || actual.has_default != expected.has_default ||
        implementation.optional_parameters.contains(actual.name) !=
            signature.optional_parameters.contains(expected.name)) {
      return false;
    }
  }
  return true;
}

bool TypeChecker::IsMatchingGenericSignature(const Function& implementation,
                                             const Function& signature) {
  const auto& parameters = implementation.type_parameters;
  const auto& expected = signature.type_parameters;
  if (parameters.size() != expected.size()) {
    return false;
  }
  TypeSubstitutions substitutions;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto& target = expected.at(index);
    substitutions.emplace(parameters.at(index).identity,
                          ParameterTypeName(target, target.location));
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto& bound = parameters.at(index).bound;
    const auto& target = expected.at(index).bound;
    if (bound.has_value() != target.has_value()) {
      return false;
    }
    if (bound &&
        !IsSameType(ResolveType(SubstituteTypeName(*bound, substitutions), VoidTypePolicy::Allowed),
                    ResolveType(*target, VoidTypePolicy::Allowed))) {
      return false;
    }
  }
  FunctionInfo actual;
  actual.visibility = implementation.visibility;
  actual.storage = implementation.storage;
  actual.return_type = ResolveType(SubstituteTypeName(implementation.return_type, substitutions),
                                   VoidTypePolicy::Allowed);
  for (const auto& parameter : implementation.parameters) {
    actual.AddParameter(
        parameter.name,
        ResolveType(SubstituteTypeName(parameter.type, substitutions), VoidTypePolicy::Forbidden));
    actual.parameters.back().has_default = parameter.initializer != nullptr;
    if (parameter.initializer) {
      actual.optional_parameters.insert(parameter.name);
    }
  }
  return MatchesImplementationSignature(actual, ResolveSignature(signature));
}

void TypeChecker::ResolveInterfaces(const Program& program) {
  for (const auto& source : program.interfaces) {
    RegisterInterface(source, false);
  }
}

void TypeChecker::CheckImplementations(const Program& program) {
  for (const auto& source : program.classes) {
    MergeBaseClass(source.name);
  }
  for (const auto& source : program.classes) {
    CheckClassImplementation(source);
  }
}

void TypeChecker::CheckClassImplementation(const Class& source) {
  const auto found = classes_.find(source.name);
  if (found == classes_.end() || !merged_classes_.contains(source.name)) {
    return;
  }
  auto& owner = found->second;
  RegisterClassDispatch(owner);
  absl::flat_hash_map<std::string, std::string> applications;
  for (const auto& name : owner.interfaces) {
    const auto type = DescribeType(types_.at(name));
    const auto [application, inserted] = applications.emplace(type.name, name);
    if (!inserted && application->second != name) {
      AddDiagnostic(source.location, "conflicting applications of interface '" + type.name +
                                         "': '" + application->second + "' and '" + name + "'");
    }
    CheckInterfaceImplementation(owner, name);
  }
}

void TypeChecker::CheckInterfaceImplementation(ClassInfo& owner, const std::string& name) {
  const auto& source = *owner.declaration;
  const auto& interface = interfaces_.at(name);
  const auto& applications = method_applications_.at(name);
  bool valid = true;
  for (const auto& [method_name, signature] : interface.methods) {
    if (applications.contains(method_name)) {
      continue;
    }
    auto qualified_name = name;
    qualified_name += '.';
    qualified_name += method_name;
    const auto method = owner.methods.find(method_name);
    if (method == owner.methods.end()) {
      AddDiagnostic(source.location, "missing interface method '" + qualified_name + "'");
      valid = false;
      continue;
    }
    if (!MatchesImplementationSignature(method->second, signature)) {
      AddDiagnostic(source.location,
                    "interface method implementation does not match '" + qualified_name + "'");
      valid = false;
    } else {
      owner.virtual_targets.insert_or_assign(
          {name, method_name, DeclarationKind::Method},
          MemberReference{.owner = method->second.member_owner, .member = method_name});
    }
  }
  for (const auto& [method_name, signature] : interface.method_templates) {
    auto qualified_name = name;
    qualified_name += '.';
    qualified_name += method_name;
    const auto implementation = owner.method_templates.find(method_name);
    if (implementation == owner.method_templates.end()) {
      AddDiagnostic(source.location, "missing generic interface method '" + qualified_name + "'");
      valid = false;
    } else if (!IsMatchingGenericSignature(*implementation->second.declaration,
                                           *signature.declaration)) {
      AddDiagnostic(
          implementation->second.declaration->location,
          "generic interface method implementation does not match '" + qualified_name + "'");
      valid = false;
    }
  }
  if (!valid || owner.type.is_abstract) {
    return;
  }
  RegisterDispatchImplementer(owner, name);
}

void TypeChecker::RegisterClassDispatch(ClassInfo& owner) {
  if (!owner.declaration->native_key.empty()) {
    return;
  }
  const auto* ancestor = &owner;
  while (ancestor != nullptr) {
    if (!owner.type.is_abstract) {
      RegisterDispatchImplementer(owner, std::string(ancestor->type.name));
    }
    for (const auto& method : ancestor->declaration->methods) {
      if (!method.IsVirtualMethod() || method.application) {
        continue;
      }
      const auto& implementation = owner.methods.at(method.name);
      auto kind = DeclarationKind::Method;
      if (method.is_getter) {
        kind = DeclarationKind::Getter;
      } else if (method.is_setter) {
        kind = DeclarationKind::Setter;
      }
      const auto& target =
          method.is_setter ? implementation.setter_owner : implementation.member_owner;
      owner.virtual_targets.insert_or_assign(
          {std::string(ancestor->type.name), method.name, kind},
          MemberReference{.owner = target, .member = method.name});
    }
    ancestor = ancestor->base_class ? &classes_.at(*ancestor->base_class) : nullptr;
  }
}

void TypeChecker::MergeBaseClass(std::string_view name) {
  if (merged_classes_.contains(name)) {
    return;
  }
  if (!visiting_classes_.insert(std::string(name)).second) {
    const auto found = classes_.find(name);
    if (found != classes_.end()) {
      AddDiagnostic(found->second.declaration->location, "class inheritance cycle");
    }
    return;
  }
  const auto found = classes_.find(name);
  if (found == classes_.end()) {
    visiting_classes_.erase(name);
    return;
  }
  auto& info = found->second;
  if (info.base_class) {
    MergeBaseClass(*info.base_class);
    const auto base = classes_.find(*info.base_class);
    if (base == classes_.end() || !merged_classes_.contains(*info.base_class)) {
      visiting_classes_.erase(name);
      return;
    }
    MergeInheritedMembers(info, base->second);
  }
  ResolveClassConstructor(*info.declaration, info);
  CheckOverrides(info);
  visiting_classes_.erase(name);
  merged_classes_.insert(std::string(name));
}

void TypeChecker::MergeInheritedMembers(ClassInfo& info, const ClassInfo& base) {
  const auto own_fields = info.field_order;
  for (const auto& field_name : own_fields) {
    info.fields.at(field_name).index += static_cast<std::uint32_t>(base.field_order.size());
  }
  for (const auto& [field_name, field] : base.fields) {
    info.fields.try_emplace(field_name, field);
  }
  info.field_order = base.field_order;
  info.field_order.insert(info.field_order.end(), own_fields.begin(), own_fields.end());
  for (const auto& [method_name, method] : base.methods) {
    if (method_name == kConstructorName || method.visibility == Visibility::Private) {
      continue;
    }
    const auto found = info.methods.find(method_name);
    if (found == info.methods.end()) {
      info.methods.emplace(method_name, method);
    } else if ((found->second.is_getter || found->second.is_setter) &&
               (method.is_getter || method.is_setter)) {
      auto& implementation = found->second;
      if (method.is_getter && !implementation.is_getter) {
        MergeAccessor(implementation, AccessorSignature(method, DeclarationKind::Getter),
                      info.declaration->location);
      }
      if (method.is_setter && !implementation.is_setter) {
        MergeAccessor(implementation, AccessorSignature(method, DeclarationKind::Setter),
                      info.declaration->location);
      }
    }
  }
  for (const auto& [method_name, method] : base.method_templates) {
    if (method.declaration->visibility == Visibility::Public) {
      info.method_templates.try_emplace(method_name, method);
    }
  }
  for (const auto& interface_name : base.interfaces) {
    info.interfaces.insert(interface_name);
  }
}

TypeChecker::FunctionInfo TypeChecker::AccessorSignature(const FunctionInfo& method,
                                                         DeclarationKind kind) {
  auto result = method;
  result.is_getter = kind == DeclarationKind::Getter;
  result.is_setter = kind == DeclarationKind::Setter;
  if (result.is_getter) {
    result.parameters.clear();
    result.parameter_indices.clear();
  } else {
    result.return_type = method.setter_return_type.value_or(method.return_type);
    result.member_owner = method.setter_owner;
    result.visibility = method.setter_visibility;
  }
  return result;
}

void TypeChecker::CheckOverrides(const ClassInfo& info) {
  const auto* base = info.base_class ? &classes_.at(*info.base_class) : nullptr;
  for (const auto methods :
       {std::span(info.declaration->methods), std::span(info.declaration->method_templates)}) {
    for (const auto& method : methods) {
      CheckOverride(method, base);
    }
  }
}

void TypeChecker::CheckOverride(const Function& method, const ClassInfo* base) {
  if (method.visibility == Visibility::Private) {
    if (method.is_override) {
      AddDiagnostic(method.location, "private members cannot override base members");
    }
    return;
  }
  if (!method.type_parameters.empty()) {
    CheckGenericOverride(method, base);
    return;
  }
  if (base != nullptr && base->method_templates.contains(method.name)) {
    AddDiagnostic(method.location, "generic base method requires a generic override");
    return;
  }
  if (base == nullptr || !base->methods.contains(method.name)) {
    if (method.is_override) {
      AddDiagnostic(method.location, "override method does not override a base method");
    }
    return;
  }
  if (!method.is_override) {
    AddDiagnostic(method.location, "base method override requires 'override'");
    return;
  }
  const auto implementation = ResolveSignature(method);
  auto signature = base->methods.at(method.name);
  if (method.is_getter || method.is_setter) {
    if ((method.is_getter && !signature.is_getter) || (method.is_setter && !signature.is_setter)) {
      AddDiagnostic(method.location, "override accessor does not override a base accessor");
      return;
    }
    signature = AccessorSignature(
        signature, method.is_getter ? DeclarationKind::Getter : DeclarationKind::Setter);
  }
  if (!MatchesImplementationSignature(implementation, signature)) {
    AddDiagnostic(method.location, "override method signature does not match base method");
  }
}

void TypeChecker::CheckGenericOverride(const Function& method, const ClassInfo* base) {
  const Function* inherited = nullptr;
  if (base != nullptr) {
    const auto found = base->method_templates.find(method.name);
    if (found != base->method_templates.end()) {
      inherited = found->second.declaration;
    }
  }
  if (inherited == nullptr) {
    if (method.is_override || (base != nullptr && base->methods.contains(method.name))) {
      AddDiagnostic(method.location, "generic override requires a generic base method");
    }
  } else if (!method.is_override) {
    AddDiagnostic(method.location, "base method override requires 'override'");
  } else if (!IsMatchingGenericSignature(method, *inherited)) {
    AddDiagnostic(method.location, "generic override signature or bounds do not match base method");
  }
}

}  // namespace vela::frontend
