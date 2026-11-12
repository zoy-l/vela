#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "base/builtin.h"
#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
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
  BindJump(false_jump);
  if (statement.else_branch) {
    LowerScopedStatement(*statement.else_branch);
  }
  if (end_jump) {
    BindJump(*end_jump);
  }
}

void Lowerer::BeginControl(ControlTargets::Kind kind) {
  controls_.push_back({
      .kind = kind,
      .break_jumps = {},
      .continue_jumps = {},
      .finally_depth = finalizers_.size(),
  });
}

void Lowerer::BindContinues(std::uint32_t target) {
  for (const auto jump : controls_.back().continue_jumps) {
    current_function_->instructions.at(jump).index = target;
  }
}

void Lowerer::FinishControl(std::optional<std::size_t> exit_jump) {
  const auto control_end = static_cast<std::uint32_t>(current_function_->instructions.size());
  if (exit_jump) {
    current_function_->instructions.at(*exit_jump).index = control_end;
  }
  for (const auto jump : controls_.back().break_jumps) {
    current_function_->instructions.at(jump).index = control_end;
  }
  controls_.pop_back();
}

std::optional<std::size_t> Lowerer::LowerLoopCondition(const frontend::Statement& statement) {
  if (!statement.condition) {
    return std::nullopt;
  }
  const auto condition = LowerExpression(*statement.condition);
  if (model_.control_flow.at(&statement).condition == frontend::ConditionTruth::Unknown) {
    const auto exit_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition.reg, .location = statement.location});
    return exit_jump;
  }
  return std::nullopt;
}

void Lowerer::LowerWhile(const frontend::Statement& statement) {
  const auto loop_start = current_function_->instructions.size();
  const auto exit_jump = LowerLoopCondition(statement);
  BeginControl(ControlTargets::Kind::Loop);
  LowerScopedStatement(*statement.then_branch);
  BindContinues(static_cast<std::uint32_t>(loop_start));
  if (model_.control_flow.at(statement.then_branch.get()).can_fall_through) {
    Emit({
        .opcode = Opcode::Jump,
        .index = static_cast<std::uint32_t>(loop_start),
        .location = statement.location,
    });
  }
  FinishControl(exit_jump);
}

void Lowerer::LowerFor(const frontend::Statement& statement) {
  PushScope();
  if (statement.initializer) {
    LowerStatement(*statement.initializer);
  }
  const auto loop_start = current_function_->instructions.size();
  const auto exit_jump = LowerLoopCondition(statement);
  BeginControl(ControlTargets::Kind::Loop);
  LowerScopedStatement(*statement.then_branch);
  const auto increment_target = static_cast<std::uint32_t>(current_function_->instructions.size());
  BindContinues(increment_target);
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
  FinishControl(exit_jump);
  PopScope();
}

void Lowerer::LowerForEach(const frontend::Statement& statement) {
  PushScope();
  const auto iterable = LowerExpression(*statement.iterable);
  const auto kind = iterable.type.kind;
  const bool is_array = kind == frontend::TypeDescriptor::Kind::Array;
  RegisterId values_array = iterable.reg;
  frontend::TypeDescriptor element_type;
  if (is_array) {
    element_type = model_.array_elements.at(iterable.type.name);
  } else {
    const auto& arguments = model_.generic_arguments.at(iterable.type.name);
    element_type = arguments.front();
    const auto base = AllocateRegisterWindow(2, statement.location);
    Emit(
        {.opcode = Opcode::Move, .dst = base, .lhs = iterable.reg, .location = statement.location});
    const auto keys_array = base + 1;
    EmitSnapshotArray(Builtin::CollectionSize, base, frontend::MakeArrayType(element_type),
                      statement.location);
    EmitBuiltinCall(Builtin::CollectionKeys, AllocateRegister(statement.location), base,
                    statement.location);
    values_array = keys_array;
  }
  const auto index = AllocateRegister(statement.location);
  Emit({.opcode = Opcode::LoadInt, .dst = index, .immediate = 0, .location = statement.location});
  const auto length = AllocateRegister(statement.location);
  EmitBuiltinCall(Builtin::ArrayLength, length, values_array, statement.location);
  const auto loop_start = current_function_->instructions.size();
  const auto condition = EmitTemporary({
      .opcode = Opcode::LessInt,
      .lhs = index,
      .rhs = length,
      .location = statement.location,
  });
  const auto exit_jump = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition, .location = statement.location});
  const auto value = EmitTemporary({
      .opcode = Opcode::GetIndex,
      .lhs = values_array,
      .rhs = index,
      .location = statement.location,
  });
  RecordValueType(value, element_type);
  const auto binding = AllocateRegister(statement.location);
  Emit({.opcode = Opcode::Move, .dst = binding, .lhs = value, .location = statement.location});
  DeclareLocal(statement.name, binding, element_type, statement.location, &statement.name);
  BeginControl(ControlTargets::Kind::Loop);
  LowerScopedStatement(*statement.then_branch);
  BindContinues(static_cast<std::uint32_t>(current_function_->instructions.size()));
  const auto one = EmitTemporary({
      .opcode = Opcode::LoadInt,
      .immediate = 1,
      .location = statement.location,
  });
  Emit({
      .opcode = Opcode::AddInt,
      .dst = index,
      .lhs = index,
      .rhs = one,
      .location = statement.location,
  });
  Emit({
      .opcode = Opcode::Jump,
      .index = static_cast<std::uint32_t>(loop_start),
      .location = statement.location,
  });
  FinishControl(exit_jump);
  PopScope();
}

}  // namespace vela::ir
