#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

void Lowerer::LowerScopedStatement(const frontend::Statement& statement) {
  PushScope();
  LowerStatement(statement);
  PopScope();
}

void Lowerer::LowerIf(const frontend::Statement& statement) {
  const auto& flow = model_.control_flow.at(&statement);
  // 即使条件结果已知，也要保留条件表达式实际会执行的副作用。
  const auto condition = LowerExpression(*statement.condition);
  if (flow.condition != frontend::ConditionTruth::Unknown) {
    if (flow.condition == frontend::ConditionTruth::AlwaysTrue) {
      LowerScopedStatement(*statement.then_branch);
    } else if (statement.else_branch) {
      LowerScopedStatement(*statement.else_branch);
    }
    return;
  }
  const auto false_jump = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition.reg, .location = statement.location});
  LowerScopedStatement(*statement.then_branch);
  std::optional<std::size_t> end_jump;
  if (statement.else_branch &&
      model_.control_flow.at(statement.then_branch.get()).can_fall_through) {
    end_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = statement.location});
  }
  current_function_->instructions.at(false_jump).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
  if (statement.else_branch) {
    LowerScopedStatement(*statement.else_branch);
  }
  if (end_jump) {
    current_function_->instructions.at(*end_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
}

void Lowerer::LowerWhile(const frontend::Statement& statement) {
  const auto loop_start = current_function_->instructions.size();
  const auto condition = LowerExpression(*statement.condition);
  std::optional<std::size_t> exit_jump;
  if (model_.control_flow.at(&statement).condition == frontend::ConditionTruth::Unknown) {
    exit_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition.reg, .location = statement.location});
  }
  controls_.push_back({});
  LowerScopedStatement(*statement.then_branch);
  for (const auto jump : controls_.back().continue_jumps) {
    current_function_->instructions.at(jump).index = static_cast<std::uint32_t>(loop_start);
  }
  if (model_.control_flow.at(statement.then_branch.get()).can_fall_through) {
    Emit({
        .opcode = Opcode::Jump,
        .index = static_cast<std::uint32_t>(loop_start),
        .location = statement.location,
    });
  }
  const auto loop_end = static_cast<std::uint32_t>(current_function_->instructions.size());
  if (exit_jump) {
    current_function_->instructions.at(*exit_jump).index = loop_end;
  }
  for (const auto jump : controls_.back().break_jumps) {
    current_function_->instructions.at(jump).index = loop_end;
  }
  controls_.pop_back();
}

void Lowerer::LowerFor(const frontend::Statement& statement) {
  PushScope();
  if (statement.initializer) {
    LowerStatement(*statement.initializer);
  }
  const auto loop_start = current_function_->instructions.size();
  std::optional<std::size_t> exit_jump;
  if (statement.condition) {
    const auto condition = LowerExpression(*statement.condition);
    if (model_.control_flow.at(&statement).condition == frontend::ConditionTruth::Unknown) {
      exit_jump = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition.reg, .location = statement.location});
    }
  }
  controls_.push_back({});
  LowerScopedStatement(*statement.then_branch);
  const auto increment_target = static_cast<std::uint32_t>(current_function_->instructions.size());
  for (const auto jump : controls_.back().continue_jumps) {
    current_function_->instructions.at(jump).index = increment_target;
  }
  const auto& body = model_.control_flow.at(statement.then_branch.get());
  if (body.can_fall_through || body.can_continue) {
    if (statement.increment) {
      static_cast<void>(LowerExpression(*statement.increment));
    }
    Emit({
        .opcode = Opcode::Jump,
        .index = static_cast<std::uint32_t>(loop_start),
        .location = statement.location,
    });
  }
  const auto loop_end = static_cast<std::uint32_t>(current_function_->instructions.size());
  if (exit_jump) {
    current_function_->instructions.at(*exit_jump).index = loop_end;
  }
  for (const auto jump : controls_.back().break_jumps) {
    current_function_->instructions.at(jump).index = loop_end;
  }
  controls_.pop_back();
  PopScope();
}

}  // namespace vela::ir
