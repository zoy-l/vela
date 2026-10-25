#include "compiler/frontend/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

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
  const auto matches_signature = [](const FunctionInfo& implementation,
                                    const FunctionInfo& signature) -> bool {
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
  };
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
        if (!matches_signature(method->second, signature)) {
          AddDiagnostic(source.location,
                        "interface method implementation does not match '" + qualified_name + "'");
        }
      }
    }
  }
}

}  // namespace vela::frontend
