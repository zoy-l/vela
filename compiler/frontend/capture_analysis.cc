#include "compiler/frontend/capture_analysis.h"

#include <cstddef>
#include <ranges>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"

namespace vela::frontend {
namespace {

class CaptureAnalysis final {
 public:
  absl::flat_hash_set<const void*> Run(const Program& program);

 private:
  struct Declaration final {
    const void* node = nullptr;
    BindingKind kind = BindingKind::Mutable;
    std::size_t lambda_depth = 0;
  };
  void Walk(const Function& function);
  void Walk(const Statement* statement);
  void Walk(const Expression* expression);
  void WalkScoped(const Statement* statement);
  void Declare(const std::string& name, const void* node, BindingKind kind);

  std::vector<absl::flat_hash_map<std::string, Declaration>> scopes_;
  absl::flat_hash_set<const void*> captured_;
  std::size_t lambda_depth_ = 0;
};

void CaptureAnalysis::Declare(const std::string& name, const void* node, BindingKind kind) {
  scopes_.back().try_emplace(
      name, Declaration{.node = node, .kind = kind, .lambda_depth = lambda_depth_});
}

void CaptureAnalysis::Walk(const Function& function) {
  scopes_.emplace_back();
  for (const auto& parameter : function.parameters) {
    Declare(parameter.name, &parameter, BindingKind::Mutable);
  }
  Walk(function.body.get());
  scopes_.pop_back();
}

void CaptureAnalysis::Walk(const Expression* expression) {
  if (expression == nullptr) {
    return;
  }
  if (expression->kind == Expression::Kind::Name) {
    for (const auto& scope : scopes_ | std::views::reverse) {
      const auto found = scope.find(expression->text);
      if (found != scope.end()) {
        const auto& declaration = found->second;
        if (declaration.kind == BindingKind::Mutable && declaration.lambda_depth < lambda_depth_) {
          captured_.insert(declaration.node);
        }
        break;
      }
    }
  }
  if (expression->function) {
    ++lambda_depth_;
    Walk(*expression->function);
    --lambda_depth_;
  }
  for (const auto* child : {
           expression->left.get(),
           expression->right.get(),
           expression->operand.get(),
           expression->callee.get(),
           expression->object.get(),
           expression->index.get(),
       }) {
    Walk(child);
  }
  for (const auto& element : expression->elements) {
    Walk(element.get());
  }
  for (const auto& argument : expression->arguments) {
    Walk(argument.value.get());
  }
  for (const auto& arm : expression->arms) {
    Walk(arm.pattern.get());
    scopes_.emplace_back();
    if (arm.kind == MatchArm::Kind::Binding) {
      Declare(arm.binding, &arm, BindingKind::Mutable);
    }
    Walk(arm.value.get());
    scopes_.pop_back();
  }
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
  const bool has_scope =
      statement->kind == Statement::Kind::Block || statement->kind == Statement::Kind::For;
  if (has_scope) {
    scopes_.emplace_back();
  }
  Walk(statement->initializer.get());
  Walk(statement->expression.get());
  Walk(statement->condition.get());
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

absl::flat_hash_set<const void*> CaptureAnalysis::Run(const Program& program) {
  for (const auto& global : program.globals) {
    Walk(global.initializer.get());
  }
  for (const auto& function : program.functions) {
    Walk(function);
  }
  for (const auto& owner : program.classes) {
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
  return captured_;
}

}  // namespace

absl::flat_hash_set<const void*> FindCapturedMutableDeclarations(const Program& program) {
  return CaptureAnalysis().Run(program);
}

}  // namespace vela::frontend
