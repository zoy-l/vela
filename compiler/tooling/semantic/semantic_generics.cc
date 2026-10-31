#include "compiler/tooling/semantic/semantic_index.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"

namespace vela::tooling {

const frontend::NodeFacts& SemanticIndex::Facts() const {
  return phase_ == IndexPhase::Definitions ? model_.definitions : model_;
}

const frontend::NodeFacts& SemanticIndex::FactsFor(const frontend::Expression& expression) const {
  const auto& definitions = model_.definitions;
  return definitions.expression_types.contains(&expression) ||
                 definitions.bindings.contains(&expression)
             ? definitions
             : model_;
}

frontend::TypeDescriptor SemanticIndex::Capability(frontend::TypeDescriptor type) const {
  absl::flat_hash_set<frontend::TypeParameterId> visited;
  while (type.kind == frontend::TypeDescriptor::Kind::Parameter) {
    const auto& identity = type.parameter->identity;
    const auto bound = model_.parameter_bounds.find(identity);
    if (bound == model_.parameter_bounds.end() || !visited.insert(identity).second) {
      return {};
    }
    const bool known_non_null = type.known_non_null;
    const bool nullable = type.nullable || (bound->second.nullable && !type.known_non_null);
    type = bound->second;
    if (known_non_null && type.kind == frontend::TypeDescriptor::Kind::Parameter) {
      type.known_non_null = true;
    }
    if (nullable && !type.nullable) {
      type = frontend::MakeNullableType(type);
    } else if (!nullable && type.nullable) {
      type.nullable = false;
      type.name = type.kind == frontend::TypeDescriptor::Kind::Function
                      ? type.name.substr(1, type.name.size() - 3)
                      : type.name.substr(0, type.name.size() - 1);
    }
  }
  return type;
}

std::vector<std::string_view> SemanticIndex::MemberOwners(frontend::TypeDescriptor type) const {
  std::vector<std::string_view> owners;
  absl::flat_hash_set<std::string_view> visited;
  auto name = type.name;
  while (visited.insert(name).second) {
    owners.push_back(name);
    const auto owner = model_.classes.find(name);
    if (owner == model_.classes.end() || !owner->second.base_class) {
      break;
    }
    name = *owner->second.base_class;
  }
  return owners;
}

std::string SemanticIndex::DisplayType(frontend::TypeDescriptor type) const {
  using Kind = frontend::TypeDescriptor::Kind;
  if (type.kind == Kind::Parameter) {
    return type.parameter->name + (type.nullable ? "?" : "");
  }
  if (type.nullable) {
    type.nullable = false;
    type.name = type.kind == Kind::Function ? type.name.substr(1, type.name.size() - 3)
                                            : type.name.substr(0, type.name.size() - 1);
    return (type.kind == Kind::Function ? "(" + DisplayType(type) + ")" : DisplayType(type)) + '?';
  }
  if (const auto application = model_.type_applications.find(type.name);
      application != model_.type_applications.end()) {
    return DisplaySymbolName(application->second.ToString());
  }
  if (const auto element = model_.array_elements.find(type.name);
      element != model_.array_elements.end()) {
    return DisplayType(element->second) + "[]";
  }
  if (const auto function = model_.function_types.find(type.name);
      function != model_.function_types.end()) {
    std::string text = "(";
    for (const auto& parameter : function->second.parameters) {
      text += (text.size() == 1 ? "" : ", ") + DisplayType(parameter.type);
    }
    return text + ") => " + DisplayType(function->second.return_type);
  }
  if (const auto application = model_.generic_arguments.find(type.name);
      application != model_.generic_arguments.end()) {
    const auto& arguments = application->second;
    switch (type.kind) {
      case Kind::Future:
        return "Future<" + DisplayType(arguments.front()) + '>';
      case Kind::Completer:
        return "Completer<" + DisplayType(arguments.front()) + '>';
      case Kind::Set:
        return "Set<" + DisplayType(arguments.front()) + '>';
      case Kind::Map:
        return "Map<" + DisplayType(arguments.front()) + ", " + DisplayType(arguments.back()) + '>';
      default:
        break;
    }
  }
  return DisplaySymbolName(type.name);
}

void SemanticIndex::MarkTypeParameters(std::span<const frontend::TypeParameter> parameters) {
  for (const auto& parameter : parameters) {
    const auto detail =
        "type parameter " + parameter.name +
        (parameter.bound ? " extends " + DisplaySymbolName(parameter.bound->ToString()) : "");
    const auto symbol = AddSymbol(parameter.location, CompletionKind::Type, detail);
    symbols_.at(symbol).role = Symbol::Role::TypeParameter;
    type_parameter_symbols_.insert_or_assign(parameter.identity, symbol);
    const auto key = KeyAt(parameter.location);
    references_.insert_or_assign(key, Reference{.symbol = symbol});
    type_parameters_.insert_or_assign(key, kSemanticDeclaration);
  }
  for (const auto& parameter : parameters) {
    if (parameter.bound) {
      VisitType(*parameter.bound);
    }
  }
}

std::string SemanticIndex::DisplaySignature(const frontend::FunctionDescriptor& signature) const {
  std::string text = "(";
  for (const auto& parameter : signature.parameters) {
    text += text.size() == 1 ? "" : ", ";
    text += parameter.rest ? "..." : "";
    text += parameter.name + (signature.optional_parameters.contains(parameter.name) ? "?" : "") +
            ": " + DisplayType(parameter.type);
  }
  return text + "): " + DisplayType(signature.return_type);
}

void SemanticIndex::VisitDefinitions(const Scope& root) {
  const auto previous = phase_;
  phase_ = IndexPhase::Definitions;
  const auto& templates = program_.templates;
  for (const auto& function : templates.functions) {
    VisitFunction(function, root);
  }
  for (const auto& declaration : templates.classes) {
    VisitClass(declaration, root);
  }
  for (const auto& declaration : templates.interfaces) {
    VisitInterface(declaration, root);
  }
  for (const auto& alias : templates.aliases) {
    MarkTypeParameters(alias.type_parameters);
    VisitType(alias.target);
  }
  phase_ = previous;
}

}  // namespace vela::tooling
