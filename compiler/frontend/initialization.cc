#include "compiler/frontend/initialization.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {
namespace {

struct Initialized final {
  enum class Base : unsigned char { Pending, Complete, Maybe };
  absl::flat_hash_set<std::string> fields;
  Base base = Base::Pending;
};
using Path = std::optional<Initialized>;

Path Merge(Path left, const Path& right) {
  if (!left) {
    return right;
  }
  if (right) {
    absl::erase_if(left->fields,
                   [&](const auto& name) -> bool { return !right->fields.contains(name); });
    if (left->base != right->base) {
      left->base = Initialized::Base::Maybe;
    }
  }
  return left;
}

bool IsThis(const Expression& expression) {
  return expression.kind == Expression::Kind::Name && expression.text == "this";
}

class Initialization final {
 public:
  Initialization(const Class& owner, const SemanticModel& model) : owner_(owner), model_(model) {}
  ~Initialization() = default;
  Initialization(const Initialization&) = delete;
  Initialization& operator=(const Initialization&) = delete;
  Initialization(Initialization&&) = delete;
  Initialization& operator=(Initialization&&) = delete;
  Diagnostics Run();

 private:
  struct Control final {
    enum class Kind : unsigned char { Loop, Switch };
    Kind kind = Kind::Loop;
    Path breaks;
    Path continues;
    std::size_t finally_depth = 0;
  };
  struct Finally final {
    const Statement* body = nullptr;
    std::size_t control_depth = 0;
  };
  void RequireComplete(const Initialized& initialized, SourceLocation location);
  void Error(SourceLocation location, std::string message);
  void Walk(const Expression* expression, Initialized& initialized);
  void WalkSuper(const Expression& expression, Initialized& initialized);
  bool WalkSpecial(const Expression& expression, Initialized& initialized);
  Path Walk(const Statement& statement, Initialized initialized);
  Path Loop(const Statement& statement, Initialized initialized);
  Path Switch(const Statement& statement, Initialized initialized);
  Path Try(const Statement& statement, const Initialized& initialized);
  Path ApplyFinalizers(Initialized initialized, std::size_t depth);

  const Class& owner_;
  const SemanticModel& model_;
  Initialized fields_;
  Diagnostics diagnostics_;
  std::vector<Control> controls_;
  std::vector<Finally> finalizers_;
};

void Initialization::Error(SourceLocation location, std::string message) {
  diagnostics_.push_back({.location = location, .message = std::move(message), .source_path = {}});
}

void Initialization::RequireComplete(const Initialized& initialized, SourceLocation location) {
  if (owner_.base_class && initialized.base != Initialized::Base::Complete) {
    Error(location, "derived constructor must call super(...) before this escapes or returns");
  }
  for (const auto& field : owner_.fields) {
    if (field.storage == MemberStorage::Instance && !initialized.fields.contains(field.name)) {
      Error(location, "field '" + field.name +
                          "' must be initialized before this escapes or the constructor returns");
    }
  }
}

void Initialization::WalkSuper(const Expression& expression, Initialized& initialized) {
  for (const auto& argument : expression.arguments) {
    Walk(argument.value.get(), initialized);
  }
  if (initialized.base != Initialized::Base::Pending) {
    Error(expression.location, "super(...) must be called exactly once on each constructor path");
  }
  initialized.base = Initialized::Base::Complete;
  const auto base_class = owner_.base_class;
  if (!base_class) {
    return;
  }
  const auto& base = model_.classes.at(base_class->name);
  initialized.fields.insert(base.field_order.begin(), base.field_order.end());
}

bool Initialization::WalkSpecial(const Expression& expression, Initialized& initialized) {
  if (expression.kind == Expression::Kind::Call &&
      expression.callee->kind == Expression::Kind::Name && expression.callee->text == "super") {
    WalkSuper(expression, initialized);
    return true;
  }
  if (expression.kind == Expression::Kind::Lambda) {
    const auto& lambda = model_.lambdas.at(model_.lambda_indices.at(&expression));
    if (std::ranges::any_of(lambda.captures, [](const Capture& capture) -> bool {
          return capture.name == "this";
        })) {
      RequireComplete(initialized, expression.location);
    }
    return true;
  }
  if (expression.kind == Expression::Kind::Match) {
    Walk(expression.operand.get(), initialized);
    Path merged;
    for (const auto& arm : expression.arms) {
      auto branch = initialized;
      Walk(arm.value.get(), branch);
      merged = Merge(std::move(merged), branch);
    }
    if (merged) {
      initialized = std::move(*merged);
    }
    return true;
  }
  if (expression.kind == Expression::Kind::Binary && expression.text == "=" &&
      expression.left->kind == Expression::Kind::Member && IsThis(*expression.left->object)) {
    Walk(expression.right.get(), initialized);
    initialized.fields.insert(expression.left->text);
    return true;
  }
  if (expression.kind == Expression::Kind::Member && IsThis(*expression.object)) {
    const bool is_field = fields_.fields.contains(expression.text);
    if (is_field && !initialized.fields.contains(expression.text)) {
      Error(expression.location, "field '" + expression.text + "' is read before initialization");
    } else if (!is_field) {
      RequireComplete(initialized, expression.location);
    }
    return true;
  }
  if (expression.kind == Expression::Kind::Binary &&
      (expression.text == "&&" || expression.text == "||")) {
    Walk(expression.left.get(), initialized);
    auto right = initialized;
    Walk(expression.right.get(), right);
    initialized = *Merge(initialized, right);
    return true;
  }
  return false;
}

void Initialization::Walk(const Expression* expression, Initialized& initialized) {
  if (expression == nullptr) {
    return;
  }
  if (IsThis(*expression)) {
    RequireComplete(initialized, expression->location);
    return;
  }
  if (WalkSpecial(*expression, initialized)) {
    return;
  }
  for (const auto* child : {
           expression->left.get(),
           expression->right.get(),
           expression->operand.get(),
           expression->callee.get(),
           expression->object.get(),
           expression->index.get(),
       }) {
    Walk(child, initialized);
  }
  for (const auto& argument : expression->arguments) {
    Walk(argument.value.get(), initialized);
  }
  for (const auto& element : expression->elements) {
    Walk(element.get(), initialized);
  }
}

Path Initialization::Loop(const Statement& statement, Initialized initialized) {
  if (statement.initializer) {
    auto after = Walk(*statement.initializer, std::move(initialized));
    if (!after) {
      return std::nullopt;
    }
    initialized = std::move(*after);
  }
  Walk(statement.condition.get(), initialized);
  const auto control = controls_.size();
  controls_.push_back({
      .kind = Control::Kind::Loop,
      .breaks = std::nullopt,
      .continues = std::nullopt,
      .finally_depth = finalizers_.size(),
  });
  auto body = Walk(*statement.then_branch, initialized);
  body = Merge(std::move(body), controls_.at(control).continues);
  if (body) {
    Walk(statement.increment.get(), *body);
  }
  auto exits = std::move(controls_.at(control).breaks);
  controls_.pop_back();
  const bool always = model_.control_flow.at(&statement).condition == ConditionTruth::AlwaysTrue;
  return always ? exits : Merge(std::move(exits), initialized);
}

Path Initialization::Switch(const Statement& statement, Initialized initialized) {
  Walk(statement.condition.get(), initialized);
  const auto control = controls_.size();
  controls_.push_back({
      .kind = Control::Kind::Switch,
      .breaks = std::nullopt,
      .continues = std::nullopt,
      .finally_depth = finalizers_.size(),
  });
  Path previous;
  bool has_default = false;
  for (const auto& branch : statement.cases) {
    has_default = has_default || !branch.label;
    previous = Walk(*branch.body, *Merge(initialized, previous));
  }
  auto exits = Merge(std::move(previous), controls_.at(control).breaks);
  controls_.pop_back();
  return has_default ? exits : Merge(std::move(exits), initialized);
}

Path Initialization::Walk(const Statement& statement, Initialized initialized) {
  switch (statement.kind) {
    case Statement::Kind::Block: {
      Path current = std::move(initialized);
      for (const auto& child : statement.statements) {
        if (!current) {
          break;
        }
        current = Walk(*child, std::move(*current));
      }
      return current;
    }
    case Statement::Kind::If: {
      Walk(statement.condition.get(), initialized);
      const auto condition = model_.control_flow.at(&statement).condition;
      if (condition == ConditionTruth::AlwaysTrue) {
        return Walk(*statement.then_branch, std::move(initialized));
      }
      if (condition == ConditionTruth::AlwaysFalse) {
        return statement.else_branch ? Walk(*statement.else_branch, std::move(initialized))
                                     : Path(std::move(initialized));
      }
      auto left = Walk(*statement.then_branch, initialized);
      auto right =
          statement.else_branch ? Walk(*statement.else_branch, initialized) : Path(initialized);
      return Merge(std::move(left), right);
    }
    case Statement::Kind::While:
    case Statement::Kind::For:
      return Loop(statement, std::move(initialized));
    case Statement::Kind::ForEach:
      Walk(statement.iterable.get(), initialized);
      return Walk(*statement.then_branch, std::move(initialized));
    case Statement::Kind::Switch:
      return Switch(statement, std::move(initialized));
    case Statement::Kind::Try:
      return Try(statement, initialized);
    case Statement::Kind::Throw:
      Walk(statement.expression.get(), initialized);
      return std::nullopt;
    case Statement::Kind::Return:
      Walk(statement.expression.get(), initialized);
      if (const auto completed = ApplyFinalizers(std::move(initialized), 0)) {
        RequireComplete(*completed, statement.location);
      }
      return std::nullopt;
    case Statement::Kind::Break: {
      const auto target = controls_.size() - 1;
      const auto completed =
          ApplyFinalizers(std::move(initialized), controls_.back().finally_depth);
      auto& control = controls_.at(target);
      control.breaks = Merge(std::move(control.breaks), completed);
      return std::nullopt;
    }
    case Statement::Kind::Continue: {
      auto target = controls_.size() - 1;
      while (controls_.at(target).kind != Control::Kind::Loop) {
        --target;
      }
      const auto completed =
          ApplyFinalizers(std::move(initialized), controls_.at(target).finally_depth);
      auto& control = controls_.at(target);
      control.continues = Merge(std::move(control.continues), completed);
      return std::nullopt;
    }
    case Statement::Kind::VariableDeclaration:
    case Statement::Kind::Expression:
      Walk(statement.expression.get(), initialized);
      return initialized;
  }
  std::unreachable();
}

Path Initialization::ApplyFinalizers(Initialized initialized, std::size_t depth) {
  if (depth == finalizers_.size()) {
    return initialized;
  }
  const auto saved = finalizers_;
  Path current(std::move(initialized));
  for (auto index = saved.size(); index > depth && current; --index) {
    const auto final = saved.at(index - 1);
    finalizers_.resize(index - 1);
    std::vector<Control> hidden(
        std::make_move_iterator(controls_.begin() +
                                static_cast<std::ptrdiff_t>(final.control_depth)),
        std::make_move_iterator(controls_.end()));
    controls_.resize(final.control_depth);
    current = Walk(*final.body, std::move(*current));
    controls_.insert(controls_.end(), std::make_move_iterator(hidden.begin()),
                     std::make_move_iterator(hidden.end()));
  }
  finalizers_ = saved;
  return current;
}

Path Initialization::Try(const Statement& statement, const Initialized& initialized) {
  if (statement.finally_branch) {
    finalizers_.push_back(
        {.body = statement.finally_branch.get(), .control_depth = controls_.size()});
  }
  auto after = Walk(*statement.then_branch, initialized);
  for (const auto& clause : statement.catches) {
    after = Merge(std::move(after), Walk(*clause.body, initialized));
  }
  if (statement.finally_branch) {
    finalizers_.pop_back();
    // 异常可在字段写入之前发生；finally 的异常入口不继承普通出口的初始化事实。
    static_cast<void>(Walk(*statement.finally_branch, initialized));
    if (after) {
      after = Walk(*statement.finally_branch, std::move(*after));
    }
  }
  return after;
}

Diagnostics Initialization::Run() {
  Initialized initialized;
  const auto& info = model_.classes.at(owner_.name);
  fields_.fields.insert(info.field_order.begin(), info.field_order.end());
  if (owner_.base_class && !model_.explicit_super_constructors.contains(owner_.name)) {
    const auto& base = model_.classes.at(owner_.base_class->name);
    initialized.base = Initialized::Base::Complete;
    initialized.fields.insert(base.field_order.begin(), base.field_order.end());
  }
  for (const auto& field : owner_.fields) {
    if (field.storage == MemberStorage::Instance) {
      if (field.initializer || field.role == FieldRole::Parameter) {
        initialized.fields.insert(field.name);
      }
    }
  }
  if (owner_.constructor) {
    const auto exits = Walk(*owner_.constructor->body, std::move(initialized));
    if (exits) {
      RequireComplete(*exits, owner_.constructor->location);
    }
  } else if (owner_.base_class) {
    RequireComplete(initialized, owner_.location);
  }
  return std::move(diagnostics_);
}

}  // namespace

Diagnostics CheckConstructorInitialization(const Class& owner, const SemanticModel& model) {
  return Initialization(owner, model).Run();
}

}  // namespace vela::frontend
