#include "compiler/frontend/typing/type_inference.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

void TypeInference::AddCandidate(const TypeParameterId& parameter,
                                 const TypeName& type,
                                 Evidence evidence) {
  if (type.name == kNullTypeName || HasUnspecifiedType(type)) {
    return;
  }
  auto& candidates = variables_.at(parameter).candidates;
  const auto key = CanonicalTypeText(type);
  const auto [found, inserted] = candidates.try_emplace(
      key, Candidate{.type = type, .from_bound = evidence == Evidence::Bound});
  if (evidence == Evidence::Bound) {
    found->second.from_bound = true;
  }
  if (inserted) {
    bound_work_.push_back({.parameter = parameter, .type = type});
  }
}

void TypeInference::Bind(const TypeParameter& parameter,
                         const TypeName& type,
                         Evidence evidence,
                         Relation relation) {
  auto& variable = variables_.at(parameter.identity);
  if (!variable.binding_keys.emplace(CanonicalTypeText(type), evidence, relation).second) {
    return;
  }
  variable.bindings.push_back({.type = type, .relation = relation});
  variable.fixed = variable.fixed || evidence == Evidence::Explicit;
  AddCandidate(parameter.identity, type, evidence);
  // null 本身不确定类型；与其它输入共同出现时仅产生已有的可空合并候选。
  const bool has_null = std::ranges::any_of(variable.bindings, [](const Binding& binding) {
    return binding.relation == Relation::Input && binding.type.name == kNullTypeName;
  });
  if (!has_null) {
    return;
  }
  std::vector<Candidate> nullable;
  for (const auto& [key, candidate] : variable.candidates) {
    static_cast<void>(key);
    if (candidate.type.name != kVoidTypeName && !IsNullableTypeName(candidate.type)) {
      const TypeName null{
          .name = std::string(kNullTypeName),
          .location = {},
          .arguments = {},
          .parameter = {},
      };
      nullable.push_back(
          {.type = *CommonType(candidate.type, null), .from_bound = candidate.from_bound});
    }
  }
  for (const auto& candidate : nullable) {
    AddCandidate(parameter.identity, candidate.type,
                 candidate.from_bound ? Evidence::Bound : evidence);
  }
}

void TypeInference::Propagate() {
  while (!bound_work_.empty()) {
    auto work = std::move(bound_work_.back());
    bound_work_.pop_back();
    const auto& parameter = parameters_.at(parameter_indices_.at(work.parameter));
    if (!parameter.bound ||
        !propagated_bounds_.emplace(work.parameter, CanonicalTypeText(work.type)).second) {
      continue;
    }
    // 仅已有实参证据触发传播；声明上界不会单独为无证据的参数产生默认实参。
    Match(*parameter.bound, work.type, Evidence::Bound, Relation::Bound);
  }
}

bool TypeInference::IsValidCandidate(const Variable& variable, Candidate& candidate) const {
  const bool allows_subtype = variable.fixed || candidate.from_bound;
  if (candidate.allows_subtype != allows_subtype) {
    candidate.allows_subtype = allows_subtype;
    candidate.checked_bindings = 0;
    candidate.valid = true;
  }
  if (!candidate.valid) {
    return false;
  }
  // 新证据只验证尚未处理的关系；候选获得上界投影证据时才重验原有输入关系。
  while (candidate.checked_bindings < variable.bindings.size()) {
    const auto& binding = variable.bindings.at(candidate.checked_bindings++);
    if (!IsValidBinding(candidate, binding)) {
      candidate.valid = false;
      return false;
    }
  }
  return true;
}

bool TypeInference::IsValidBinding(const Candidate& candidate, const Binding& binding) const {
  switch (binding.relation) {
    case Relation::Equal:
      return IsSame(candidate.type, binding.type);
    case Relation::Context:
      return IsValidConstraint(candidate.type, binding.type, Relation::Context);
    case Relation::Conversion:
      return CanBindType(candidate.type, binding.type);
    case Relation::Bound:
      return IsStaticSubtype(binding.type, candidate.type);
    case Relation::Input: {
      const auto common = CommonType(candidate.type, binding.type);
      const bool inferred = common && IsSame(*common, candidate.type);
      return inferred || (candidate.allows_subtype && CanBindType(candidate.type, binding.type));
    }
    case Relation::Callback:
      return IsValidConstraint(candidate.type, binding.type, Relation::Callback);
  }
  std::unreachable();
}

bool TypeInference::IsStaticSubtype(const TypeName& subtype, const TypeName& supertype) const {
  const auto key = std::pair(CanonicalTypeText(subtype), CanonicalTypeText(supertype));
  if (const auto found = static_relations_.find(key); found != static_relations_.end()) {
    return found->second;
  }
  const bool result = relations_->IsStaticSubtype(subtype, supertype);
  static_relations_.emplace(key, result);
  return result;
}

bool TypeInference::CanBindType(const TypeName& destination, const TypeName& source) const {
  const auto key = std::pair(CanonicalTypeText(destination), CanonicalTypeText(source));
  if (const auto found = binding_relations_.find(key); found != binding_relations_.end()) {
    return found->second;
  }
  const bool result = relations_->CanBindType(destination, source);
  binding_relations_.emplace(key, result);
  return result;
}

bool TypeInference::IsValidConstraint(const TypeName& pattern,
                                      const TypeName& actual,
                                      Relation relation) const {
  if (actual.kind == TypeName::Kind::Named && actual.name.empty()) {
    return true;
  }
  if (relation != Relation::Callback && !HasUnspecifiedType(actual)) {
    if (relation == Relation::Equal) {
      return IsSame(pattern, actual);
    }
    return relation == Relation::Context ? CanBindType(actual, pattern)
                                         : CanBindType(pattern, actual);
  }
  if (!IsSameTypeConstructor(pattern, actual) ||
      pattern.arguments.size() != actual.arguments.size()) {
    return false;
  }
  for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
    const auto argument_relation = ArgumentRelation(pattern, relation, index);
    if (!IsValidConstraint(pattern.arguments.at(index), actual.arguments.at(index),
                           argument_relation)) {
      return false;
    }
  }
  return true;
}

void TypeInference::SelectCandidates() {
  conflicts_.clear();
  for (const auto& parameter : parameters_) {
    auto& variable = variables_.at(parameter.identity);
    variable.type.reset();
    std::vector<const TypeName*> valid;
    for (auto& [key, candidate] : variable.candidates) {
      static_cast<void>(key);
      if (IsValidCandidate(variable, candidate)) {
        valid.push_back(&candidate.type);
      }
    }
    const TypeName* selected = nullptr;
    for (const auto* candidate : valid) {
      const bool most_specific = std::ranges::all_of(valid, [&](const TypeName* other) {
        return IsSame(*candidate, *other) || IsStaticSubtype(*candidate, *other);
      });
      if (most_specific) {
        selected = candidate;
        break;
      }
    }
    if (selected != nullptr) {
      variable.type = *selected;
    } else if (!variable.candidates.empty()) {
      conflicts_.push_back(parameter.identity);
    }
  }
}

bool TypeInference::IsMissingBoundProjection(const TypeName& actual, const TypeName& bound) const {
  const auto& requirement =
      bound.kind == TypeName::Kind::Nullable ? bound.arguments.front() : bound;
  if (requirement.kind == TypeName::Kind::Parameter) {
    return false;
  }
  const auto& provided =
      actual.kind == TypeName::Kind::Nullable ? actual.arguments.front() : actual;
  return !IsSameTypeConstructor(provided, requirement) &&
         SupertypeProjections(provided, requirement).empty();
}

std::expected<TypeInference::Solution, TypeInference::Failure> TypeInference::Solve() const {
  if (!conflicts_.empty()) {
    const auto& parameter = parameters_.at(parameter_indices_.at(conflicts_.front()));
    const auto& bindings = variables_.at(parameter.identity).bindings;
    const auto& actual = bindings.front().type;
    const auto different = std::ranges::find_if(
        bindings, [&](const Binding& binding) { return !IsSame(actual, binding.type); });
    return std::unexpected(Failure{
        .kind = Failure::Kind::Conflicting,
        .parameter = parameter.name,
        .actual = actual,
        .required = different == bindings.end() ? actual : different->type,
    });
  }
  for (const auto& parameter : parameters_) {
    const auto& actual = variables_.at(parameter.identity).type;
    if (!parameter.bound || !actual) {
      continue;
    }
    const auto bound = Substitute(*parameter.bound);
    const bool failed = bound ? !IsStaticSubtype(*actual, *bound)
                              : IsMissingBoundProjection(*actual, *parameter.bound);
    if (failed) {
      return std::unexpected(Failure{
          .kind = Failure::Kind::Bound,
          .parameter = parameter.name,
          .actual = *actual,
          .required = bound ? *bound : *parameter.bound,
      });
    }
  }
  Solution result;
  for (const auto& parameter : parameters_) {
    const auto& variable = variables_.at(parameter.identity);
    if (!variable.type) {
      return std::unexpected(Failure{
          .kind = Failure::Kind::Unresolved,
          .parameter = parameter.name,
          .actual = {},
          .required = {},
      });
    }
    result.variables_.emplace(parameter.identity, *variable.type);
  }
  for (const auto& constraint : constraints_) {
    const auto applied = result.Substitute(constraint.pattern);
    const auto& actual = constraint.actual;
    const auto relation = EvidenceRelation(constraint.evidence);
    const bool valid = IsValidConstraint(applied, actual, relation);
    if (!valid) {
      return std::unexpected(Failure{
          .kind = Failure::Kind::Relation,
          .parameter = {},
          .actual = actual,
          .required = applied,
      });
    }
  }
  return result;
}

}  // namespace vela::frontend
