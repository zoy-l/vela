#include "compiler/frontend/type_checker.h"

#include <string>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"

namespace vela::frontend {

auto TypeChecker::CheckTry(const Statement& statement, const Type& return_type) -> StatementFlow {
  auto flow = CheckScopedStatement(*statement.then_branch, return_type);
  absl::flat_hash_set<std::string> caught;
  for (const auto& clause : statement.catches) {
    // 异常可发生在任意副作用之后，catch 不能沿用 try 的普通出口事实。
    refinements_.clear();
    BeginScope();
    if (!clause.name.empty()) {
      const auto type = ResolveType(clause.type, false);
      if ((type.kind != Type::Kind::Class && type.kind != Type::Kind::Interface) || type.nullable) {
        AddDiagnostic(clause.location, "catch type must be a non-null class or interface");
      }
      if (!caught.insert(type.name).second) {
        AddDiagnostic(clause.location, "duplicate catch type '" + type.name + "'");
      }
      catch_types_.emplace(&clause, type);
      if (DeclareLocal(clause.name, type, clause.location, BindingKind::Mutable, &clause)) {
        bindings_.emplace(&clause, LookupLocal(clause.name)->id);
      }
    }
    const auto next = CheckStatement(*clause.body, return_type);
    EndScope();
    flow.can_fall_through = flow.can_fall_through || next.can_fall_through;
    flow.can_break = flow.can_break || next.can_break;
    flow.can_continue = flow.can_continue || next.can_continue;
  }
  refinements_.clear();
  if (statement.finally_branch) {
    const auto final = CheckScopedStatement(*statement.finally_branch, return_type);
    if (!final.can_fall_through) {
      flow = final;
    } else {
      flow.can_break = flow.can_break || final.can_break;
      flow.can_continue = flow.can_continue || final.can_continue;
    }
  }
  return flow;
}

}  // namespace vela::frontend
