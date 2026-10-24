#include "compiler/frontend/type_checker.h"

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"

namespace vela::frontend {

void TypeChecker::CheckCaseLabel(const Expression& label,
                                 const Type& selector_type,
                                 absl::flat_hash_set<std::string>& labels) {
  const auto actual = CheckExpression(label, &selector_type);
  if (IsValid(actual) && !IsSameType(selector_type, actual)) {
    AddDiagnostic(label.location, "case label type must match switch value type");
  }
  const auto constant = EvaluateConstant(label);
  if (!constant) {
    AddDiagnostic(label.location, "case label must be a compile-time constant");
    return;
  }
  const auto key = std::visit(
      [](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return value;
        } else {
          return std::to_string(value);
        }
      },
      *constant);
  if (!labels.insert(key).second) {
    AddDiagnostic(label.location, "duplicate case label");
  }
  case_values_.insert_or_assign(&label, *constant);
}

auto TypeChecker::CheckSwitch(const Statement& statement, const Type& return_type)
    -> StatementFlow {
  const auto type = CheckExpression(*statement.condition, nullptr);
  if (type.nullable || (type.kind != Type::Kind::Int && type.kind != Type::Kind::Bool &&
                        type.kind != Type::Kind::String && type.kind != Type::Kind::Enum)) {
    AddDiagnostic(statement.condition->location, "switch requires int, bool, string, or enum");
  }
  InvalidateAssignments(statement);
  const auto before = non_null_;
  absl::flat_hash_set<std::string> labels;
  bool has_default = false;
  std::vector<StatementFlow> branches;
  ++switch_depth_;
  for (const auto& branch : statement.cases) {
    if (branch.label) {
      CheckCaseLabel(*branch.label, type, labels);
    } else if (has_default) {
      AddDiagnostic(branch.location, "duplicate default label");
    } else {
      has_default = true;
    }
    non_null_ = before;
    branches.push_back(CheckStatement(*branch.body, return_type));
  }
  --switch_depth_;
  non_null_ = before;
  StatementFlow result{.can_fall_through = !has_default};
  bool tail_falls_through = true;
  for (auto branch = branches.rbegin(); branch != branches.rend(); ++branch) {
    tail_falls_through = branch->can_break || (branch->can_fall_through && tail_falls_through);
    result.can_fall_through = result.can_fall_through || tail_falls_through;
    result.can_continue = result.can_continue || branch->can_continue;
  }
  return result;
}

}  // namespace vela::frontend
