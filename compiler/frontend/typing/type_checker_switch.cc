#include "compiler/frontend/typing/type_checker.h"

#include <cstddef>
#include <ranges>
#include <string>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::frontend {

TypeChecker::CheckStatus TypeChecker::CheckConstantLabel(const Expression& label,
                                                         const Type& selector_type,
                                                         absl::flat_hash_set<std::string>& labels,
                                                         SourceLocation location,
                                                         ConstantLabelKind kind) {
  const bool match = kind == ConstantLabelKind::Match;
  const auto actual = CheckExpression(label, &selector_type);
  if (IsValid(actual) && !IsSameType(selector_type, actual)) {
    AddDiagnostic(location, match ? "match pattern type must equal the selector type"
                                  : "case label type must match switch value type");
  }
  auto constant = EvaluateConstant(label);
  if (!constant) {
    AddDiagnostic(location, match ? "match pattern must be a compile-time constant"
                                  : "case label must be a compile-time constant");
    return CheckStatus::Failed;
  }
  const auto key = ConstantKey(*constant);
  if (!labels.insert(key).second) {
    AddDiagnostic(location, match ? "duplicate match pattern" : "duplicate case label");
  }
  facts_.case_values.insert_or_assign(&label, *constant);
  return CheckStatus::Succeeded;
}

StatementFlow TypeChecker::CheckSwitch(const Statement& statement, const Type& return_type) {
  const auto type = CheckExpression(*statement.condition, nullptr);
  if (type.nullable || (type.kind != Type::Kind::Int && type.kind != Type::Kind::Bool &&
                        type.kind != Type::Kind::String && type.kind != Type::Kind::Enum)) {
    AddDiagnostic(statement.condition->location, "switch requires int, bool, string, or enum");
  }
  InvalidateAssignments(statement);
  absl::flat_hash_set<std::string> labels;
  bool has_default = false;
  std::vector<StatementFlow> branches;
  ++switch_depth_;
  const auto& cases = statement.cases;
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto& branch = cases.at(index);
    const auto checkpoint = refinements_.BeginBranch();
    if (branch.label) {
      static_cast<void>(CheckConstantLabel(*branch.label, type, labels, branch.label->location,
                                           ConstantLabelKind::Switch));
    } else if (has_default) {
      AddDiagnostic(branch.location, "duplicate default label");
    } else {
      has_default = true;
    }
    const auto flow = CheckStatement(*branch.body, return_type);
    branches.push_back(flow);
    if (index + 1 < cases.size() && !branch.body->statements.empty() && flow.can_fall_through) {
      AddDiagnostic(cases.at(index + 1).location,
                    "non-empty switch branch must not fall through to the next label");
    }
    refinements_.Discard(checkpoint);
  }
  --switch_depth_;
  StatementFlow result{.can_fall_through = !has_default};
  bool tail_falls_through = true;
  for (const auto& branch : std::views::reverse(branches)) {
    tail_falls_through = branch.can_break || (branch.can_fall_through && tail_falls_through);
    result.can_fall_through = result.can_fall_through || tail_falls_through;
    result.can_continue = result.can_continue || branch.can_continue;
  }
  return result;
}

}  // namespace vela::frontend
