#include "compiler/frontend/type_inference.h"
#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_names.h"

namespace vela::frontend {

bool TypeInference::IsSame(const TypeName& left, const TypeName& right) {
  if (!IsSameTypeConstructor(left, right) || left.arguments.size() != right.arguments.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.arguments.size(); ++index) {
    if (!IsSame(left.arguments.at(index), right.arguments.at(index))) {
      return false;
    }
  }
  return true;
}

bool TypeInference::HasUnspecifiedType(const TypeName& type) {
  return (type.kind == TypeName::Kind::Named && type.name.empty()) ||
         std::ranges::any_of(type.arguments, HasUnspecifiedType);
}

namespace {

TypeName Nullable(TypeName type) {
  if (IsNullableTypeName(type)) {
    return type;
  }
  TypeName result{
      .name = {},
      .location = type.location,
      .kind = TypeName::Kind::Nullable,
      .arguments = {},
      .parameter = {},
  };
  result.arguments.push_back(std::move(type));
  return result;
}

}  // namespace

TypeParameter IntrinsicTypeParameter(std::string_view name) {
  return {
      .name = std::string(name),
      .location = {},
      .bound = std::nullopt,
      .identity = {.owner = std::string(name), .index = 0},
  };
}

TypeName IntrinsicTypeVariable(std::string_view name, SourceLocation location) {
  return ParameterTypeName(IntrinsicTypeParameter(name), location);
}

TypeInference::TypeInference(std::span<const TypeParameter> parameters,
                             InferenceTypeRelations& relations)
    : relations_(&relations), parameters_(parameters.begin(), parameters.end()) {
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto& parameter = parameters.subspan(index).front();
    variables_.try_emplace(parameter.identity);
    parameter_indices_.emplace(parameter.identity, index);
  }
}

const TypeParameter* TypeInference::FindParameter(const TypeName& pattern) const {
  if (pattern.kind != TypeName::Kind::Parameter) {
    return nullptr;
  }
  const auto found = parameter_indices_.find(pattern.parameter);
  return found == parameter_indices_.end() ? nullptr : &parameters_.at(found->second);
}

std::optional<TypeName> TypeInference::CommonType(const TypeName& left, const TypeName& right) {
  if (IsSame(left, right)) {
    return left;
  }
  if (left.name == kNullTypeName && right.name != kVoidTypeName) {
    return Nullable(right);
  }
  if (right.name == kNullTypeName && left.name != kVoidTypeName) {
    return Nullable(left);
  }
  if (left.kind == TypeName::Kind::Nullable && IsSame(left.arguments.front(), right)) {
    return left;
  }
  if (right.kind == TypeName::Kind::Nullable && IsSame(left, right.arguments.front())) {
    return right;
  }
  return std::nullopt;
}

void TypeInference::Constrain(const TypeName& pattern, const TypeName& actual, Evidence evidence) {
  constraints_.push_back({.pattern = pattern, .actual = actual, .evidence = evidence});
  const auto relation = EvidenceRelation(evidence);
  Match(pattern, actual, evidence, evidence == Evidence::Explicit ? Relation::Equal : relation);
  Propagate();
  SelectCandidates();
}

TypeInference::Relation TypeInference::EvidenceRelation(Evidence evidence) {
  switch (evidence) {
    case Evidence::Context:
      return Relation::Context;
    case Evidence::Callback:
      return Relation::Callback;
    default:
      return Relation::Input;
  }
}

TypeInference::Relation TypeInference::ArgumentRelation(const TypeName& pattern,
                                                        Relation relation,
                                                        std::size_t index) const {
  // 数组 intrinsic 按调用方向转换每个参数和结果；普通函数值仍使用整体不变契约。
  if (relation == Relation::Callback) {
    return index + 1 == pattern.arguments.size() ? Relation::Conversion : Relation::Context;
  }
  return relations_->IsInvariantType(pattern) ? Relation::Equal : relation;
}

void TypeInference::Match(const TypeName& pattern,
                          const TypeName& actual,
                          Evidence evidence,
                          Relation relation) {
  if (actual.kind == TypeName::Kind::Named && actual.name.empty()) {
    return;
  }
  if (const auto* parameter = FindParameter(pattern)) {
    Bind(*parameter, actual, evidence, relation);
    return;
  }
  if (pattern.kind == TypeName::Kind::Nullable) {
    if (actual.name != kNullTypeName) {
      Match(pattern.arguments.front(),
            actual.kind == TypeName::Kind::Nullable ? actual.arguments.front() : actual, evidence,
            relation);
    }
    return;
  }
  if (actual.kind == TypeName::Kind::Nullable) {
    Match(pattern, actual.arguments.front(), evidence, relation);
    return;
  }
  if (!IsSameTypeConstructor(pattern, actual)) {
    const auto projections = relation == Relation::Context ? SupertypeProjections(pattern, actual)
                                                           : SupertypeProjections(actual, pattern);
    for (const auto& projection : projections) {
      if (relation == Relation::Context) {
        Match(projection, actual, evidence, relation);
      } else {
        Match(pattern, projection, evidence, relation);
      }
    }
    return;
  }
  if (pattern.arguments.size() != actual.arguments.size()) {
    return;
  }
  for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
    const auto argument_relation = ArgumentRelation(pattern, relation, index);
    Match(pattern.arguments.at(index), actual.arguments.at(index), evidence, argument_relation);
  }
}

std::span<const TypeName> TypeInference::SupertypeProjections(const TypeName& source,
                                                              const TypeName& constructor) const {
  const auto key = std::pair(CanonicalTypeText(source), CanonicalTypeText(constructor));
  if (const auto found = projections_.find(key); found != projections_.end()) {
    return found->second;
  }
  auto projected = relations_->ProjectSupertypes({.subtype = &source, .supertype = &constructor});
  return projections_.emplace(key, std::move(projected)).first->second;
}

std::optional<TypeName> TypeInference::Substitute(const TypeName& pattern) const {
  if (FindParameter(pattern) != nullptr) {
    return variables_.at(pattern.parameter).type;
  }
  TypeName result = pattern;
  for (auto& argument : result.arguments) {
    auto resolved = Substitute(argument);
    if (!resolved) {
      return std::nullopt;
    }
    argument = std::move(*resolved);
  }
  if (pattern.kind == TypeName::Kind::Nullable &&
      FindParameter(pattern.arguments.front()) != nullptr &&
      IsNullableTypeName(result.arguments.front())) {
    return std::move(result.arguments.front());
  }
  return result;
}

bool TypeInference::CanResolveFrom(std::span<const TypeName> inputs) const {
  absl::flat_hash_set<TypeParameterId> available;
  std::vector<TypeParameterId> pending;
  const auto collect = [&](const auto& self, const TypeName& type) -> void {
    if (const auto* parameter = FindParameter(type)) {
      if (available.insert(parameter->identity).second) {
        pending.push_back(parameter->identity);
      }
    }
    for (const auto& argument : type.arguments) {
      self(self, argument);
    }
  };
  for (const auto& parameter : parameters_) {
    const auto& variable = variables_.at(parameter.identity);
    if (variable.type && variable.type->name != kNullTypeName) {
      collect(collect, ParameterTypeName(parameter, parameter.location));
    }
  }
  for (const auto& input : inputs) {
    collect(collect, input);
  }
  while (!pending.empty()) {
    auto identity = std::move(pending.back());
    pending.pop_back();
    const auto& parameter = parameters_.at(parameter_indices_.at(identity));
    if (parameter.bound) {
      collect(collect, *parameter.bound);
    }
  }
  return available.size() == parameters_.size();
}

TypeName TypeInference::Solution::Substitute(TypeName pattern) const {
  return SubstituteTypeName(std::move(pattern), variables_);
}

}  // namespace vela::frontend
