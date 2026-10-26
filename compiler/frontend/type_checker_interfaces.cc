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

void TypeChecker::ResolveInterfaces(const Program& program) {
  std::uint32_t index = 0;
  for (const auto& source : program.interfaces) {
    InterfaceDescriptor interface;
    interface.type = {.kind = Type::Kind::Interface, .name = source.name};
    interface.index = interface_ids_ == nullptr ? index++ : interface_ids_->Get(source.name);
    for (const auto& method : source.methods) {
      if (!interface.methods.emplace(method.name, ResolveSignature(method)).second) {
        AddDiagnostic(method.location, "duplicate interface method '" + method.name + "'");
      }
    }
    interfaces_.try_emplace(source.name, std::move(interface));
  }
}

void TypeChecker::CheckImplementations(const Program& program) {
  absl::flat_hash_set<std::string> merged;
  absl::flat_hash_set<std::string> visiting;
  for (const auto& source : program.classes) {
    if (source.base_class) {
      MergeBaseClass(source.name, merged, visiting);
    }
  }
  for (const auto& source : program.classes) {
    const auto found = classes_.find(source.name);
    if (found == classes_.end()) {
      continue;
    }
    const auto& owner = found->second;
    for (const auto& name : owner.interfaces) {
      for (const auto& [method_name, signature] : interfaces_.at(name).methods) {
        auto qualified_name = name;
        qualified_name += '.';
        qualified_name += method_name;
        const auto method = owner.methods.find(method_name);
        if (method == owner.methods.end()) {
          AddDiagnostic(source.location, "missing interface method '" + qualified_name + "'");
          continue;
        }
        if (!MatchesImplementationSignature(method->second, signature)) {
          AddDiagnostic(source.location,
                        "interface method implementation does not match '" + qualified_name + "'");
        }
      }
    }
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
  for (const auto& interface_name : base.interfaces) {
    info.interfaces.insert(interface_name);
  }
}

void TypeChecker::CheckOverrides(const ClassInfo& info) {
  for (const auto& method : info.declaration->methods) {
    const auto base = info.base_class ? classes_.find(*info.base_class) : classes_.end();
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
}

}  // namespace vela::frontend
