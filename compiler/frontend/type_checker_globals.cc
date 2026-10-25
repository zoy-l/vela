#include "compiler/frontend/type_checker.h"

#include <string>
#include <utility>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"

namespace vela::frontend {

void TypeChecker::ResolveGlobals(const Program& program) {
  for (const auto& declaration : program.globals) {
    if (IsBuiltinName(declaration.name) || functions_.contains(declaration.name) ||
        types_.contains(declaration.name) || aliases_.contains(declaration.name) ||
        !globals_
             .emplace(declaration.name,
                      GlobalDescriptor{.type = Type(), .declaration = &declaration})
             .second) {
      AddDiagnostic(declaration.location,
                    "duplicate or reserved global '" + declaration.name + "'");
    }
  }
  for (const auto& declaration : program.globals) {
    if (globals_.contains(declaration.name)) {
      static_cast<void>(ResolveGlobal(declaration.name));
    }
  }
}

auto TypeChecker::ResolveGlobal(const std::string& name) -> Type {
  auto& global = globals_.at(name);
  const auto& declaration = *global.declaration;
  if (checked_globals_.contains(name)) {
    return global.type;
  }
  if (checking_globals_.contains(name)) {
    if (!IsValid(global.type)) {
      AddDiagnostic(declaration.location,
                    "cyclic global type inference requires an explicit type annotation");
    }
    return global.type;
  }
  checking_globals_.insert(name);
  if (!declaration.type.name.empty()) {
    global.type = ResolveType(declaration.type, false);
  }
  const auto actual =
      CheckGlobalInitializer(declaration, IsValid(global.type) ? &global.type : nullptr);
  if (declaration.type.name.empty()) {
    if (actual.kind == Type::Kind::Null || actual.kind == Type::Kind::Void) {
      AddDiagnostic(declaration.location, "global initializer requires an explicit value type");
    } else {
      global.type = actual;
    }
  } else if (IsValid(actual) && !CanAssign(global.type, actual)) {
    AddDiagnostic(declaration.location, "global initializer does not match its declared type");
  }
  checking_globals_.erase(name);
  checked_globals_.insert(name);
  return global.type;
}

auto TypeChecker::CheckGlobalInitializer(const Global& declaration, const Type* expected) -> Type {
  // 递归推导另一个全局声明时不能捕获当前 lambda 的局部变量或控制流事实。
  auto saved_scopes = std::move(scopes_);
  auto saved_lambdas = std::move(lambda_scopes_);
  auto saved_facts = std::move(refinements_);
  const auto* const saved_owner = std::exchange(owner_, nullptr);
  scopes_.clear();
  lambda_scopes_.clear();
  refinements_.clear();
  const auto actual = CheckExpression(*declaration.initializer, expected);
  scopes_ = std::move(saved_scopes);
  lambda_scopes_ = std::move(saved_lambdas);
  refinements_ = std::move(saved_facts);
  owner_ = saved_owner;
  return actual;
}

}  // namespace vela::frontend
