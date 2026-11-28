#include "compiler/frontend/flow/capture_analysis.h"

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::frontend {
namespace {

class CaptureAnalysis final {
 public:
  explicit CaptureAnalysis(CapturedDeclarations& captured) : captured_(&captured) {}
  void Run(const Program& program);
  void Run(const Function& function);
  void Run(const Class& owner);

 private:
  struct Declaration final {
    const void* node = nullptr;
    BindingKind kind = BindingKind::Mutable;
    std::size_t lambda_depth = 0;
  };
  enum class Access : std::uint8_t { Read, Write };
  void RecordReference(const Expression& expression, Access access);
  void Walk(const Function& function);
  void Walk(const Class& owner);
  void Walk(const Statement* statement);
  void Walk(const Expression* expression, Access access = Access::Read);
  void WalkScoped(const Statement* statement);
  void Declare(const std::string& name, const void* node, BindingKind kind);

  std::vector<absl::flat_hash_map<std::string, Declaration>> scopes_;
  CapturedDeclarations* captured_;
  std::size_t lambda_depth_ = 0;
};

void CaptureAnalysis::Declare(const std::string& name, const void* node, BindingKind kind) {
  scopes_.back().try_emplace(
      name, Declaration{.node = node, .kind = kind, .lambda_depth = lambda_depth_});
}

void CaptureAnalysis::Walk(const Function& function) {
  scopes_.emplace_back();
  for (const auto& parameter : function.parameters) {
    Walk(parameter.initializer.get());
    if (parameter.binding == Parameter::Binding::Local) {
      Declare(parameter.name, &parameter, BindingKind::Mutable);
    }
  }
  Walk(function.body.get());
  scopes_.pop_back();
}

void CaptureAnalysis::RecordReference(const Expression& expression, Access access) {
  for (const auto& scope : scopes_ | std::views::reverse) {
    const auto found = scope.find(expression.text);
    if (found == scope.end()) {
      continue;
    }
    const auto& declaration = found->second;
    if (declaration.kind == BindingKind::Mutable) {
      if (declaration.lambda_depth < lambda_depth_) {
        captured_->mutables.insert(declaration.node);
      }
      if (access == Access::Write) {
        captured_->assigned.insert(declaration.node);
      }
    }
    return;
  }
}

void CaptureAnalysis::Walk(const Expression* expression, Access access) {
  if (expression == nullptr) {
    return;
  }
  if (expression->kind == Expression::Kind::Name) {
    RecordReference(*expression, access);
    return;
  }
  if ((expression->kind == Expression::Kind::Binary && expression->operation == TokenKind::Equal) ||
      expression->kind == Expression::Kind::CompoundAssignment) {
    Walk(expression->left.get(), Access::Write);
    Walk(expression->right.get());
    return;
  }
  if (expression->kind == Expression::Kind::Unary &&
      (expression->operation == TokenKind::PlusPlus ||
       expression->operation == TokenKind::MinusMinus)) {
    Walk(expression->operand.get(), Access::Write);
    return;
  }
  if (expression->function) {
    ++lambda_depth_;
    Walk(*expression->function);
    --lambda_depth_;
  }
  if (expression->kind == Expression::Kind::Match) {
    Walk(expression->operand.get());
    for (const auto& arm : expression->arms) {
      Walk(arm.pattern.get());
      scopes_.emplace_back();
      if (arm.kind == MatchArm::Kind::Binding) {
        Declare(arm.binding, &arm, BindingKind::Mutable);
      }
      Walk(arm.value.get());
      scopes_.pop_back();
    }
    return;
  }
  VisitExpressionChildren(*expression, [this](const Expression* child) { Walk(child); });
}

void CaptureAnalysis::WalkScoped(const Statement* statement) {
  if (statement != nullptr) {
    scopes_.emplace_back();
    Walk(statement);
    scopes_.pop_back();
  }
}

void CaptureAnalysis::Walk(const Statement* statement) {
  if (statement == nullptr) {
    return;
  }
  const bool has_scope = statement->IntroducesScope();
  if (has_scope) {
    scopes_.emplace_back();
  }
  Walk(statement->initializer.get());
  Walk(statement->expression.get());
  Walk(statement->condition.get());
  Walk(statement->iterable.get());
  if (statement->kind == Statement::Kind::ForEach) {
    Declare(statement->name, &statement->name, BindingKind::Constant);
  }
  WalkScoped(statement->then_branch.get());
  WalkScoped(statement->else_branch.get());
  WalkScoped(statement->finally_branch.get());
  for (const auto& clause : statement->catches) {
    scopes_.emplace_back();
    if (!clause.name.empty()) {
      Declare(clause.name, &clause, BindingKind::Mutable);
    }
    Walk(clause.body.get());
    scopes_.pop_back();
  }
  Walk(statement->increment.get());
  for (const auto& child : statement->statements) {
    Walk(child.get());
  }
  for (const auto& branch : statement->cases) {
    Walk(branch.label.get());
    Walk(branch.body.get());
  }
  if (statement->kind == Statement::Kind::VariableDeclaration) {
    Declare(statement->name, statement, statement->binding);
  }
  if (has_scope) {
    scopes_.pop_back();
  }
}

void CaptureAnalysis::Run(const Program& program) {
  for (const auto& global : program.globals) {
    Walk(global.initializer.get());
  }
  for (const auto& function : program.functions) {
    Walk(function);
  }
  for (const auto& owner : program.classes) {
    Walk(owner);
  }
}

void CaptureAnalysis::Run(const Function& function) {
  Walk(function);
}

void CaptureAnalysis::Walk(const Class& owner) {
  for (const auto& field : owner.fields) {
    Walk(field.initializer.get());
  }
  for (const auto& method : owner.methods) {
    Walk(method);
  }
  if (owner.constructor) {
    Walk(*owner.constructor);
  }
}

void CaptureAnalysis::Run(const Class& owner) {
  Walk(owner);
}

}  // namespace

void CollectCapturedDeclarations(const Program& program, CapturedDeclarations& declarations) {
  CaptureAnalysis(declarations).Run(program);
}

void CollectCapturedDeclarations(const Function& function, CapturedDeclarations& declarations) {
  CaptureAnalysis(declarations).Run(function);
}

void CollectCapturedDeclarations(const Class& owner, CapturedDeclarations& declarations) {
  CaptureAnalysis(declarations).Run(owner);
}

}  // namespace vela::frontend
