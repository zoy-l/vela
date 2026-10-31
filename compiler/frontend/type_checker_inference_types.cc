#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_inference.h"
#include "compiler/frontend/type_names.h"

namespace vela::frontend {

TypeInference TypeChecker::MakeInference(std::span<const TypeParameter> parameters) {
  std::vector<TypeParameter> normalized(parameters.begin(), parameters.end());
  for (auto& parameter : normalized) {
    if (parameter.bound) {
      parameter.bound =
          specializer_->DescribeType(specializer_->ExpandAlias(std::move(*parameter.bound)));
    }
  }
  return {normalized, *this};
}

bool TypeChecker::IsStaticSubtype(const TypeName& subtype, const TypeName& supertype) {
  const auto source = ResolveType(subtype, true);
  const auto target = ResolveType(supertype, true);
  return IsSubtype(source, target);
}

bool TypeChecker::CanBindType(const TypeName& destination, const TypeName& source) {
  const auto expected = ResolveType(destination, true);
  const auto actual = ResolveType(source, true);
  return CanAssign(expected, actual);
}

bool TypeChecker::IsInvariantType(const TypeName& type) const {
  return type.kind != TypeName::Kind::Generic || type.name != kFutureTypeName;
}

std::vector<TypeName> TypeChecker::ProjectSupertypes(const Projection& projection) {
  const auto& subtype = *projection.subtype;
  const auto& supertype = *projection.supertype;
  using Kind = TypeName::Kind;
  if (subtype.kind == Kind::Generic) {
    if (specializer_->ClassTemplate(subtype.name) == nullptr &&
        specializer_->InterfaceTemplate(subtype.name) == nullptr) {
      return {};
    }
  } else if (subtype.kind == Kind::Named) {
    const auto found = types_.find(subtype.name);
    if (found == types_.end() ||
        (found->second.kind != Type::Kind::Class && found->second.kind != Type::Kind::Interface)) {
      return {};
    }
  } else if (subtype.kind != Kind::Parameter) {
    return {};
  }
  std::vector<TypeName> result;
  std::vector<Type> pending{ResolveType(subtype, true)};
  absl::flat_hash_set<std::string_view> visited;
  while (!pending.empty()) {
    const auto current = NonNullable(pending.back());
    pending.pop_back();
    if (!IsValid(current) || !visited.insert(current.name).second) {
      continue;
    }
    const auto described = DescribeType(current);
    if (IsSameTypeConstructor(described, supertype)) {
      result.push_back(described);
    }
    if (const auto* parameter = FindTypeParameter(current)) {
      static_cast<void>(ResolveParameterBound(*parameter));
    }
    const auto parents = DirectSupertypes(current);
    pending.insert(pending.end(), parents.begin(), parents.end());
  }
  return result;
}

void TypeChecker::DiagnoseInferenceFailure(const TypeInference::Failure& failure,
                                           SourceLocation location) {
  using Kind = TypeInference::Failure::Kind;
  switch (failure.kind) {
    case Kind::Unresolved:
      AddDiagnostic(location, "cannot infer type argument '" + failure.parameter +
                                  "'; provide type arguments or a type context");
      break;
    case Kind::Conflicting:
      AddDiagnostic(location, "conflicting inferred types for '" + failure.parameter + "': '" +
                                  failure.actual.ToString() + "' and '" +
                                  failure.required.ToString() + "'");
      break;
    case Kind::Bound:
      AddDiagnostic(location, "type argument '" + failure.actual.ToString() +
                                  "' does not satisfy the bound '" + failure.required.ToString() +
                                  "' of generic parameter '" + failure.parameter + "'");
      break;
    case Kind::Relation:
      AddDiagnostic(location, "inferred type application '" + failure.required.ToString() +
                                  "' does not match type evidence '" + failure.actual.ToString() +
                                  "'");
      break;
  }
}

}  // namespace vela::frontend
