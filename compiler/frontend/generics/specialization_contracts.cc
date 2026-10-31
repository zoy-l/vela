#include "compiler/frontend/generics/specialization.h"

#include <span>
#include <string>
#include <utility>

#include "base/declaration.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

void Specializer::DescribeParameters(DeclarationOrigin& origin,
                                     std::span<const TypeParameter> parameters) {
  origin.type_parameters.assign(parameters.begin(), parameters.end());
  for (auto& parameter : origin.type_parameters) {
    if (parameter.bound) {
      parameter.bound = ExpandAlias(std::move(*parameter.bound));
    }
  }
}

std::string Specializer::DescribeFunctionContract(const Function& function) {
  TypeName signature{
      .name = {},
      .location = function.location,
      .kind = TypeName::Kind::Function,
      .arguments = {},
      .parameter = {},
  };
  for (const auto& parameter : function.parameters) {
    signature.arguments.push_back(parameter.type);
  }
  signature.arguments.push_back(function.return_type);
  const auto owner = function.type_parameters.empty()
                         ? std::string()
                         : function.type_parameters.front().identity.owner;
  return DescribeTypeContract(ExpandAlias(std::move(signature)), owner, program_.origins);
}

DeclarationOrigin::Member Specializer::DescribeMethod(const Function& method,
                                                      DeclarationKind kind) {
  DeclarationOrigin parameters;
  DescribeParameters(parameters, method.type_parameters);
  return {
      .kind = kind,
      .name = method.name,
      .has_initializer = false,
      .type_parameters =
          DescribeTypeParameterContracts(parameters.type_parameters, program_.origins),
      .generic_signature = method.type_parameters.empty() && kind != DeclarationKind::Selector
                               ? std::string()
                               : DescribeFunctionContract(method),
  };
}

void Specializer::DescribeClassTemplate(const Class& owner) {
  auto& description = program_.origins.at(owner.name);
  DescribeParameters(description, owner.type_parameters);
  auto& members = description.members;
  members.clear();
  members.push_back({
      .kind = DeclarationKind::Constructor,
      .name = "$constructor",
      .type_parameters = {},
      .generic_signature = {},
  });
  for (const auto& field : owner.fields) {
    members.push_back({
        .kind = field.storage == MemberStorage::Static ? DeclarationKind::StaticField
                                                       : DeclarationKind::Field,
        .name = field.name,
        .has_initializer = field.initializer != nullptr,
        .type_parameters = {},
        .generic_signature = {},
    });
  }
  for (const auto& method : owner.methods) {
    auto kind = DeclarationKind::Method;
    if (method.is_getter) {
      kind = DeclarationKind::Getter;
    } else if (method.is_setter) {
      kind = DeclarationKind::Setter;
    } else if (method.storage == MemberStorage::Static) {
      kind = DeclarationKind::StaticMethod;
    }
    members.push_back(DescribeMethod(method, kind));
  }
  for (const auto& method : owner.method_templates) {
    members.push_back(DescribeMethod(method, method.storage == MemberStorage::Static
                                                 ? DeclarationKind::StaticMethod
                                                 : DeclarationKind::Method));
  }
}

void Specializer::DescribeInterfaceTemplate(const Interface& owner) {
  auto& description = program_.origins.at(owner.name);
  DescribeParameters(description, owner.type_parameters);
  auto& members = description.members;
  members.clear();
  for (const auto& method : owner.methods) {
    members.push_back(DescribeMethod(method, DeclarationKind::Selector));
  }
  for (const auto& method : owner.method_templates) {
    members.push_back(DescribeMethod(method, DeclarationKind::Selector));
  }
}

}  // namespace vela::frontend
