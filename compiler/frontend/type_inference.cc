#include "compiler/frontend/type_inference.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_names.h"

namespace vela::frontend {
namespace {

bool IsSame(const TypeName& left, const TypeName& right) {
  if (left.kind != right.kind ||
      ((left.kind == TypeName::Kind::Named || left.kind == TypeName::Kind::Generic) &&
       left.name != right.name) ||
      left.arguments.size() != right.arguments.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.arguments.size(); ++index) {
    if (!IsSame(left.arguments.at(index), right.arguments.at(index))) {
      return false;
    }
  }
  return true;
}

TypeName Nullable(TypeName type) {
  if (type.kind == TypeName::Kind::Nullable || type.name == kDynamicTypeName) {
    return type;
  }
  TypeName result{
      .name = {},
      .location = type.location,
      .kind = TypeName::Kind::Nullable,
      .arguments = {},
  };
  result.arguments.push_back(std::move(type));
  return result;
}

}  // namespace

TypeInference::TypeInference(std::span<const std::string> parameters)
    : parameters_(parameters.begin(), parameters.end()) {
  for (const auto& parameter : parameters) {
    variables_.try_emplace(parameter);
  }
}

void TypeInference::Bind(const std::string& name, const TypeName& type, Evidence evidence) {
  auto& variable = variables_.at(name);
  if (variable.type && variable.type->name == kNullTypeName && type.name != kNullTypeName &&
      type.name != kVoidTypeName) {
    variable = {.type = Nullable(type), .evidence = evidence};
    return;
  }
  if (!variable.type || evidence > variable.evidence) {
    variable = {.type = type, .evidence = evidence};
    std::erase(conflicts_, name);
    return;
  }
  if (evidence < variable.evidence || IsSame(*variable.type, type)) {
    return;
  }
  if (auto common = CommonType(*variable.type, type)) {
    variable.type = std::move(common);
  } else if (std::ranges::find(conflicts_, name) == conflicts_.end()) {
    conflicts_.push_back(name);
  }
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
  if (actual.kind == TypeName::Kind::Named && actual.name.empty()) {
    return;
  }
  if (pattern.kind == TypeName::Kind::Named && variables_.contains(pattern.name)) {
    Bind(pattern.name, actual, evidence);
    return;
  }
  if (pattern.kind == TypeName::Kind::Nullable) {
    if (actual.name != kNullTypeName) {
      Constrain(pattern.arguments.front(),
                actual.kind == TypeName::Kind::Nullable ? actual.arguments.front() : actual,
                evidence);
    }
    return;
  }
  if (evidence == Evidence::Context && actual.kind == TypeName::Kind::Nullable) {
    Constrain(pattern, actual.arguments.front(), evidence);
    return;
  }
  if (pattern.kind != actual.kind || pattern.arguments.size() != actual.arguments.size() ||
      ((pattern.kind == TypeName::Kind::Named || pattern.kind == TypeName::Kind::Generic) &&
       pattern.name != actual.name)) {
    return;
  }
  for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
    Constrain(pattern.arguments.at(index), actual.arguments.at(index), evidence);
  }
}

std::optional<TypeName> TypeInference::Substitute(const TypeName& pattern) const {
  if (pattern.kind == TypeName::Kind::Named) {
    const auto found = variables_.find(pattern.name);
    if (found != variables_.end()) {
      return found->second.type;
    }
  }
  TypeName result = pattern;
  for (auto& argument : result.arguments) {
    auto resolved = Substitute(argument);
    if (!resolved) {
      return std::nullopt;
    }
    argument = std::move(*resolved);
  }
  return result;
}

std::vector<std::string> TypeInference::Unresolved() const {
  std::vector<std::string> result;
  for (const auto& parameter : parameters_) {
    const auto& variable = variables_.at(parameter);
    if (!variable.type || variable.type->name == kNullTypeName) {
      result.push_back(parameter);
    }
  }
  return result;
}

bool TypeInference::CanResolveFrom(std::span<const TypeName> inputs) const {
  const auto contains = [](const auto& self, const TypeName& pattern,
                           const std::string& name) -> bool {
    if (pattern.kind == TypeName::Kind::Named && pattern.name == name) {
      return true;
    }
    return std::ranges::any_of(pattern.arguments,
                               [&](const auto& argument) { return self(self, argument, name); });
  };
  return std::ranges::all_of(Unresolved(), [&](const auto& name) {
    return std::ranges::any_of(inputs,
                               [&](const auto& input) { return contains(contains, input, name); });
  });
}

std::optional<TypeInference::Solution> TypeInference::Solve() const {
  if (!conflicts_.empty()) {
    return std::nullopt;
  }
  Solution result;
  for (const auto& [name, variable] : variables_) {
    if (!variable.type || variable.type->name == kNullTypeName) {
      return std::nullopt;
    }
    result.variables_.emplace(name, *variable.type);
  }
  return result;
}

TypeName TypeInference::Solution::Substitute(TypeName pattern) const {
  return SubstituteTypeName(std::move(pattern), variables_);
}

}  // namespace vela::frontend
