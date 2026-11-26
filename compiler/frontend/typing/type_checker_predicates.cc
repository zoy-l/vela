#include "compiler/frontend/typing/type_checker.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::frontend {

void TypeChecker::RefinePredicateCall(const Expression& condition) {
  const auto target = facts_.call_targets.find(&condition);
  if (target == facts_.call_targets.end()) {
    return;
  }
  const auto order = facts_.argument_order.find(&condition);
  if (order == facts_.argument_order.end() || order->second.size() != condition.arguments.size()) {
    return;
  }
  const auto& argument_order = order->second;
  for (std::size_t index = 0; index < condition.arguments.size(); ++index) {
    const auto& argument = *condition.arguments[index].value;
    if (argument_order[index] != 0) {
      continue;
    }
    if (argument.kind != Expression::Kind::Name) {
      return;
    }
    const auto* binding = LookupLocal(argument.text);
    if (binding == nullptr || captured_mutables_.contains(binding->id)) {
      return;
    }
    const auto binding_id = binding->id;
    const auto narrowed = ResolvePredicate(target->second.signature);
    if (!narrowed) {
      return;
    }
    AssignmentEffects later;
    for (const auto& following : std::span(condition.arguments).subspan(index + 1)) {
      CollectAssignments(*following.value, later);
    }
    // 谓词约束的是已求值的实参；后续实参重写同名局部后，不能把约束套回新值。
    if (!later.names.contains(argument.text)) {
      refinements_.Set(binding_id, *narrowed);
    }
    return;
  }
}

void TypeChecker::RefineAlternatives(const Expression& condition, ConditionTruth outcome) {
  using Truth = ConditionTruth;
  const bool conditional = condition.kind == Expression::Kind::Conditional;
  const auto& selector = *(conditional ? condition.operand : condition.left);
  const auto first_outcome = conditional ? Truth::AlwaysTrue : outcome;
  const auto first = refinements_.BeginBranch();
  RefineCondition(selector, first_outcome);
  if (conditional) {
    InvalidateAssignments(*condition.left);
    RefineCondition(*condition.left, outcome);
  }
  auto first_facts = refinements_.Capture(first);
  const auto second = refinements_.BeginBranch();
  RefineCondition(selector, NegateConditionTruth(first_outcome));
  InvalidateAssignments(*condition.right);
  RefineCondition(*condition.right, outcome);
  const auto second_facts = refinements_.Capture(second);
  refinements_.Intersect(first_facts, second_facts);
  refinements_.Apply(first_facts);
}

bool TypeChecker::CanInferPredicate(const Function& function, const FunctionInfo& signature) {
  const auto kind = signature.return_type.kind;
  return !function.is_async && !function.parameters.empty() && !function.is_getter &&
         !function.is_setter && !signature.return_type.nullable &&
         (kind == Type::Kind::Bool || kind == Type::Kind::Invalid);
}

void TypeChecker::BeginPredicate(const Function& function, const FunctionInfo& signature) {
  predicate_scope_.reset();
  if (!CanInferPredicate(function, signature)) {
    return;
  }
  const auto* binding = LookupLocal(function.parameters.front().name);
  if (binding != nullptr) {
    predicate_scope_.emplace(
        PredicateScope{.binding = binding->id, .original = binding->type, .matched = std::nullopt});
  }
}

void TypeChecker::RecordPredicateReturn(const Expression& expression) {
  if (!predicate_scope_) {
    return;
  }
  auto& predicate = *predicate_scope_;
  if (predicate.assigned || captured_mutables_.contains(predicate.binding) ||
      (predicate.matched && IsSameType(*predicate.matched, predicate.original))) {
    return;
  }
  if (const auto constant = EvaluateConstant(expression)) {
    if (const auto* value = std::get_if<bool>(&*constant); value != nullptr && !*value) {
      return;
    }
  }
  const auto checkpoint = refinements_.BeginBranch();
  RefineCondition(expression, ConditionTruth::AlwaysTrue);
  const auto* refined = refinements_.Find(predicate.binding);
  const auto matched = refined == nullptr ? predicate.original : *refined;
  refinements_.Discard(checkpoint);
  if (!predicate.matched) {
    predicate.matched = matched;
    return;
  }
  if (IsSameType(matched, *predicate.matched)) {
    return;
  }
  predicate.matched = PredicateUpperBound(*predicate.matched, matched).value_or(predicate.original);
}

std::optional<TypeChecker::Type> TypeChecker::PredicateUpperBound(const Type& first,
                                                                  const Type& second) const {
  // 名义祖先只遍历一次；Future 与函数类型的结构关系仍由公共子类型规则判定。
  const bool nominal = second.kind == Type::Kind::Class || second.kind == Type::Kind::Parameter;
  absl::flat_hash_map<std::string_view, bool> ancestors;
  bool has_null = false;
  if (nominal) {
    std::vector<Type> pending{second};
    while (!pending.empty()) {
      const auto current = pending.back();
      pending.pop_back();
      has_null = has_null || current.kind == Type::Kind::Null;
      const auto [found, inserted] =
          ancestors.try_emplace(NonNullableTypeName(current), !current.nullable);
      if (!inserted && (current.nullable || found->second)) {
        continue;
      }
      found->second = !current.nullable;
      AppendDirectSupertypes(current, pending);
    }
  }
  const auto is_upper_bound = [&](const Type& candidate) {
    if (nominal &&
        (candidate.kind == Type::Kind::Class || candidate.kind == Type::Kind::Interface ||
         candidate.kind == Type::Kind::Parameter)) {
      const auto parent = ancestors.find(NonNullableTypeName(candidate));
      return (has_null && candidate.nullable) ||
             (parent != ancestors.end() && (candidate.nullable || parent->second));
    }
    return IsSubtype(second, candidate);
  };
  if (is_upper_bound(first)) {
    return first;
  }
  std::vector<Type> candidates;
  AppendDirectSupertypes(first, candidates);
  absl::flat_hash_set<std::string_view> visited;
  visited.insert(first.name);
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const auto candidate = candidates[index];
    if (!visited.insert(candidate.name).second) {
      continue;
    }
    if (is_upper_bound(candidate)) {
      return candidate;
    }
    AppendDirectSupertypes(candidate, candidates);
  }
  return std::nullopt;
}

std::optional<TypeChecker::Type> TypeChecker::FinishPredicate() {
  auto predicate = std::exchange(predicate_scope_, std::nullopt);
  if (!predicate || predicate->assigned || captured_mutables_.contains(predicate->binding) ||
      !predicate->matched || IsSameType(*predicate->matched, predicate->original) ||
      !CanAssign(predicate->original, *predicate->matched)) {
    return std::nullopt;
  }
  return predicate->matched;
}

std::optional<TypeChecker::Type> TypeChecker::ResolvePredicate(const FunctionInfo& signature) {
  if (signature.narrowed_parameter || signature.return_type.kind != Type::Kind::Bool ||
      signature.return_type.nullable || signature.declaration_body == nullptr) {
    return signature.narrowed_parameter;
  }
  const auto source = predicate_declarations_.find(signature.declaration_body);
  if (source == predicate_declarations_.end()) {
    return std::nullopt;
  }
  const auto* declaration = source->second;
  if (!signature.member_owner.empty() && declaration->IsVirtualMethod()) {
    return std::nullopt;
  }
  const auto* body = declaration->body.get();
  if (!facts_.control_flow.contains(body) && !checking_function_bodies_.contains(body)) {
    const FunctionContext context(*this);
    const auto* owner =
        signature.member_owner.empty() ? nullptr : &classes_.at(signature.member_owner);
    // 惰性检查会扩充调用目标表；仅在此处复制签名，避免跨 flat_hash_map rehash 借用。
    const auto checking = FunctionInfo(signature);
    CheckFunctionBody(*declaration, checking, owner);
  }
  if (const auto found = function_predicates_.find(body); found != function_predicates_.end()) {
    return found->second;
  }
  return std::nullopt;
}

}  // namespace vela::frontend
