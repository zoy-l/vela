#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

Lowerer::Completion Lowerer::LowerFinalizers(std::size_t depth) {
  if (depth == finalizers_.size()) {
    return Completion::Continue;
  }
  const auto saved_finalizers = finalizers_;
  const auto saved_handler = current_handler_;
  auto completion = Completion::Continue;
  for (auto index = saved_finalizers.size(); index > depth; --index) {
    const auto final = saved_finalizers.at(index - 1);
    finalizers_.resize(index - 1);
    current_handler_ = final.outer_handler;
    auto saved_scopes = scopes_;
    scopes_.resize(final.scope_depth);
    std::vector<ControlTargets> hidden_controls(
        std::make_move_iterator(controls_.begin() +
                                static_cast<std::ptrdiff_t>(final.control_depth)),
        std::make_move_iterator(controls_.end()));
    controls_.resize(final.control_depth);
    LowerScopedStatement(*final.body);
    scopes_ = std::move(saved_scopes);
    controls_.insert(controls_.end(), std::make_move_iterator(hidden_controls.begin()),
                     std::make_move_iterator(hidden_controls.end()));
    if (!model_.control_flow.at(final.body).can_fall_through) {
      completion = Completion::Terminated;
      break;
    }
  }
  finalizers_ = saved_finalizers;
  current_handler_ = saved_handler;
  return completion;
}

void Lowerer::LowerReturn(const frontend::Statement& statement) {
  RegisterId result = 0;
  if (statement.expression) {
    const auto value = LowerExpression(*statement.expression);
    if (current_function_->is_async) {
      result = LowerFuturePayload(value, current_function_->return_type, statement.location);
    } else {
      result = value.reg;
    }
    if (!finalizers_.empty()) {
      const auto snapshot = AllocateTemporary(statement.location);
      Emit(
          {.opcode = Opcode::Move, .dst = snapshot, .lhs = result, .location = statement.location});
      result = snapshot;
    }
  } else {
    result = AllocateTemporary(statement.location);
    Emit({.opcode = Opcode::LoadNull, .dst = result, .location = statement.location});
  }
  if (LowerFinalizers(0) == Completion::Continue) {
    Emit({.opcode = Opcode::Return, .dst = result, .location = statement.location});
  }
}

void Lowerer::LowerControlExit(const frontend::Statement& statement) {
  auto control = controls_.size() - 1;
  if (statement.kind == frontend::Statement::Kind::Continue) {
    while (controls_.at(control).kind != ControlTargets::Kind::Loop) {
      --control;
    }
  }
  if (LowerFinalizers(controls_.at(control).finally_depth) == Completion::Terminated) {
    return;
  }
  const auto jump = current_function_->instructions.size();
  Emit({.opcode = Opcode::Jump, .location = statement.location});
  auto& target = controls_.at(control);
  if (statement.kind == frontend::Statement::Kind::Break) {
    target.break_jumps.push_back(jump);
  } else {
    target.continue_jumps.push_back(jump);
  }
}

void Lowerer::LowerTry(const frontend::Statement& statement) {
  const auto outer_handler = current_handler_;
  const auto finally_depth = finalizers_.size();
  if (statement.finally_branch) {
    finalizers_.push_back({
        .body = statement.finally_branch.get(),
        .scope_depth = scopes_.size(),
        .control_depth = controls_.size(),
        .outer_handler = outer_handler,
    });
  }
  const auto handler = static_cast<std::uint32_t>(current_function_->handlers.size());
  current_function_->handlers.push_back({});
  const auto exception = AllocateTemporary(statement.location);
  std::vector<std::size_t> exits;
  const auto finish = [&]() -> void {
    if (LowerFinalizers(finally_depth) == Completion::Continue) {
      exits.push_back(current_function_->instructions.size());
      Emit({.opcode = Opcode::Jump, .location = statement.location});
    }
  };
  current_handler_ = handler;
  LowerScopedStatement(*statement.then_branch);
  if (model_.control_flow.at(statement.then_branch.get()).can_fall_through) {
    finish();
  }
  current_handler_ = outer_handler;
  current_function_->handlers.at(handler).target =
      static_cast<std::uint32_t>(current_function_->instructions.size());
  Emit({.opcode = Opcode::Catch, .dst = exception, .location = statement.location});
  const auto payload = AllocateTemporary(statement.location);
  Emit({
      .opcode = Opcode::ExceptionValue,
      .dst = payload,
      .lhs = exception,
      .location = statement.location,
  });
  std::optional<std::uint32_t> catch_finalizer;
  if (statement.finally_branch && !statement.catches.empty()) {
    catch_finalizer = static_cast<std::uint32_t>(current_function_->handlers.size());
    current_function_->handlers.push_back({});
  }
  for (const auto& clause : statement.catches) {
    std::optional<std::size_t> mismatch;
    if (!clause.name.empty()) {
      const auto condition = AllocateTemporary(clause.location);
      Emit({
          .opcode = Opcode::IsType,
          .dst = condition,
          .lhs = payload,
          .index = RegisterRuntimeType(model_.catch_types.at(&clause)),
          .location = clause.location,
      });
      mismatch = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition, .location = clause.location});
    }
    current_handler_ = catch_finalizer ? catch_finalizer : outer_handler;
    PushScope();
    if (!clause.name.empty()) {
      // 各 catch 的局部绑定具有各自的类型，不能改写共享异常载荷寄存器的类型。
      const auto bound = AllocateTemporary(clause.location);
      Emit({.opcode = Opcode::Move, .dst = bound, .lhs = payload, .location = clause.location});
      DeclareLocal(clause.name, bound, model_.catch_types.at(&clause), clause.location, &clause);
    }
    LowerStatement(*clause.body);
    PopScope();
    if (model_.control_flow.at(clause.body.get()).can_fall_through) {
      finish();
    }
    current_handler_ = outer_handler;
    if (mismatch) {
      current_function_->instructions.at(*mismatch).index =
          static_cast<std::uint32_t>(current_function_->instructions.size());
    }
  }
  if ((statement.catches.empty() || !statement.catches.back().name.empty()) &&
      LowerFinalizers(finally_depth) == Completion::Continue) {
    Emit({.opcode = Opcode::Rethrow, .lhs = exception, .location = statement.location});
  }
  if (catch_finalizer) {
    current_function_->handlers.at(*catch_finalizer).target =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    const auto replacement = AllocateTemporary(statement.location);
    Emit({.opcode = Opcode::Catch, .dst = replacement, .location = statement.location});
    if (LowerFinalizers(finally_depth) == Completion::Continue) {
      Emit({.opcode = Opcode::Rethrow, .lhs = replacement, .location = statement.location});
    }
  }
  finalizers_.resize(finally_depth);
  current_handler_ = outer_handler;
  const auto end = static_cast<std::uint32_t>(current_function_->instructions.size());
  for (const auto exit : exits) {
    current_function_->instructions.at(exit).index = end;
  }
}

}  // namespace vela::ir
