#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include "compiler/frontend/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

bool TypeChecker::MatchesImplementationSignature(const FunctionInfo& implementation,
                                                 const FunctionInfo& signature) {
  if (implementation.visibility != Visibility::Public ||
      implementation.storage != MemberStorage::Instance ||
      !IsSameType(implementation.return_type, signature.return_type) ||
      implementation.parameters.size() != signature.parameters.size()) {
    return false;
  }
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    if (!IsSameType(implementation.parameters.at(index).type,
                    signature.parameters.at(index).type)) {
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
    if (bound && !IsSameType(ResolveType(SubstituteTypeName(*bound, substitutions), true),
                             ResolveType(*target, true))) {
      return false;
    }
  }
  FunctionInfo actual;
  actual.visibility = implementation.visibility;
  actual.storage = implementation.storage;
  actual.return_type =
      ResolveType(SubstituteTypeName(implementation.return_type, substitutions), true);
  for (const auto& parameter : implementation.parameters) {
    actual.AddParameter(parameter.name,
                        ResolveType(SubstituteTypeName(parameter.type, substitutions), false));
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
    MergeBaseClass(source.name, merged_classes_, visiting_classes_);
  }
  for (const auto& source : program.classes) {
    CheckClassImplementation(source);
  }
}

void TypeChecker::CheckClassImplementation(const Class& source) {
  const auto found = classes_.find(source.name);
  if (found == classes_.end()) {
    return;
  }
  auto& owner = found->second;
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
  bool valid = true;
  for (const auto& [method_name, signature] : interface.methods) {
    if (interface.method_applications.contains(method_name)) {
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
      owner.interface_targets.insert_or_assign(
          {name, method_name},
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
    } else {
      owner.interface_method_templates.insert_or_assign({name, method_name},
                                                        implementation->second);
    }
  }
  if (!valid || owner.type.is_abstract) {
    return;
  }
  if (registered_implementers_.emplace(name, source.name).second) {
    interface_implementers_.try_emplace(name).first->second.push_back(source.name);
  }
  for (const auto& [method_name, method] : interface.method_applications) {
    static_cast<void>(method_name);
    QueueInterfaceBinding(owner, name, *method);
  }
}

void TypeChecker::MergeBaseClass(std::string_view name,
                                 absl::flat_hash_set<std::string>& merged,
                                 absl::flat_hash_set<std::string>& visiting) {
  if (merged.contains(name)) {
    return;
  }
  if (!visiting.insert(std::string(name)).second) {
    const auto found = classes_.find(name);
    if (found != classes_.end()) {
      AddDiagnostic(found->second.declaration->location, "class inheritance cycle");
    }
    return;
  }
  auto found = classes_.find(name);
  if (found == classes_.end()) {
    visiting.erase(name);
    return;
  }
  auto& info = found->second;
  if (info.base_class) {
    MergeBaseClass(*info.base_class, merged, visiting);
    const auto base = classes_.find(*info.base_class);
    if (base != classes_.end() && !visiting.contains(*info.base_class)) {
      MergeInheritedMembers(info, base->second);
    }
  }
  CheckOverrides(info);
  visiting.erase(name);
  merged.insert(std::string(name));
}

void TypeChecker::MergeInheritedMembers(ClassInfo& info, const ClassInfo& base) {
  const auto own_fields = info.field_order;
  for (const auto& field_name : own_fields) {
    info.fields.at(field_name).index += static_cast<std::uint32_t>(base.field_order.size());
  }
  absl::flat_hash_map<std::string, ClassDescriptor::Field> fields = base.fields;
  for (const auto& field_name : own_fields) {
    fields.insert_or_assign(field_name, info.fields.at(field_name));
  }
  info.fields = std::move(fields);
  info.field_order = base.field_order;
  info.field_order.insert(info.field_order.end(), own_fields.begin(), own_fields.end());
  for (const auto& [method_name, method] : base.methods) {
    if (!info.methods.contains(method_name)) {
      info.methods.emplace(method_name, method);
    }
  }
  for (const auto& [method_name, method] : base.method_templates) {
    info.method_templates.try_emplace(method_name, method);
  }
  for (const auto& interface_name : base.interfaces) {
    info.interfaces.insert(interface_name);
  }
}

void TypeChecker::CheckOverrides(const ClassInfo& info) {
  const auto base = info.base_class ? classes_.find(*info.base_class) : classes_.end();
  for (const auto& method : info.declaration->methods) {
    if (base != classes_.end() && base->second.method_templates.contains(method.name)) {
      AddDiagnostic(method.location, "generic base method requires a generic override");
      continue;
    }
    if (base == classes_.end() || !base->second.methods.contains(method.name)) {
      if (method.is_override) {
        AddDiagnostic(method.location, "override method does not override a base method");
      }
      continue;
    }
    if (!method.is_override) {
      AddDiagnostic(method.location, "base method override requires 'override'");
      continue;
    }
    const auto implementation = ResolveSignature(method);
    const auto& signature = base->second.methods.at(method.name);
    if (!MatchesImplementationSignature(implementation, signature)) {
      AddDiagnostic(method.location, "override method signature does not match base method");
    }
  }
  CheckGenericOverrides(info, base == classes_.end() ? nullptr : &base->second);
}

void TypeChecker::CheckGenericOverrides(const ClassInfo& info, const ClassInfo* base) {
  for (const auto& method : info.declaration->method_templates) {
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
      AddDiagnostic(method.location,
                    "generic override signature or bounds do not match base method");
    }
  }
}

}  // namespace vela::frontend
