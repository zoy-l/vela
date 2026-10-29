#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "base/builtin.h"
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
  controls_.push_back(
      {.break_jumps = {}, .continue_jumps = {}, .finally_depth = finalizers_.size()});
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
  controls_.push_back(
      {.break_jumps = {}, .continue_jumps = {}, .finally_depth = finalizers_.size()});
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

void Lowerer::LowerForEach(const frontend::Statement& statement) {
  PushScope();
  const auto iterable = LowerExpression(*statement.iterable);
  const auto kind = iterable.type.kind;
  const bool is_array = kind == frontend::TypeDescriptor::Kind::Array;
  const bool is_map = kind == frontend::TypeDescriptor::Kind::Map;
  RegisterId values_array = iterable.reg;
  RegisterId map_values_array = iterable.reg;
  frontend::TypeDescriptor element_type;
  frontend::TypeDescriptor value_type;
  if (is_array) {
    element_type = model_.array_elements.at(iterable.type.name);
  } else {
    const auto& arguments = model_.generic_arguments.at(iterable.type.name);
    element_type = arguments.front();
    const auto base = AllocateRegisterWindow(2, statement.location);
    Emit(
        {.opcode = Opcode::Move, .dst = base, .lhs = iterable.reg, .location = statement.location});
    const auto keys_array = base + 1;
    RegisterId value_base = base;
    if (is_map && statement.destructuring) {
      value_base = AllocateRegisterWindow(2, statement.location);
      map_values_array = value_base + 1;
      value_type = arguments.back();
      Emit({
          .opcode = Opcode::Move,
          .dst = value_base,
          .lhs = iterable.reg,
          .location = statement.location,
      });
    }
    const auto length = EmitTemporary({
        .opcode = Opcode::BuiltinCall,
        .lhs = base,
        .index = static_cast<std::uint32_t>(Builtin::CollectionSize),
        .location = statement.location,
    });
    Emit({
        .opcode = Opcode::NewDynamicArray,
        .dst = keys_array,
        .lhs = length,
        .index = RegisterArrayType(frontend::MakeArrayType(element_type)),
        .location = statement.location,
    });
    Emit({
        .opcode = Opcode::BuiltinCall,
        .dst = AllocateTemporary(statement.location),
        .lhs = base,
        .index = static_cast<std::uint32_t>(Builtin::CollectionKeys),
        .location = statement.location,
    });
    values_array = keys_array;
    if (is_map && statement.destructuring) {
      Emit({
          .opcode = Opcode::NewDynamicArray,
          .dst = map_values_array,
          .lhs = length,
          .index = RegisterArrayType(frontend::MakeArrayType(value_type)),
          .location = statement.location,
      });
      Emit({
          .opcode = Opcode::BuiltinCall,
          .dst = AllocateTemporary(statement.location),
          .lhs = value_base,
          .index = static_cast<std::uint32_t>(Builtin::CollectionValues),
          .location = statement.location,
      });
    }
  }
  const auto index = AllocateTemporary(statement.location);
  Emit({.opcode = Opcode::LoadInt, .dst = index, .immediate = 0, .location = statement.location});
  const auto length = EmitTemporary({
      .opcode = Opcode::BuiltinCall,
      .lhs = values_array,
      .index = static_cast<std::uint32_t>(Builtin::ArrayLength),
      .location = statement.location,
  });
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
  if (statement.destructuring) {
    const auto second = EmitTemporary({
        .opcode = Opcode::GetIndex,
        .lhs = map_values_array,
        .rhs = index,
        .location = statement.location,
    });
    RecordValueType(second, value_type);
    const auto second_binding = AllocateRegister(statement.location);
    Emit({
        .opcode = Opcode::Move,
        .dst = second_binding,
        .lhs = second,
        .location = statement.location,
    });
    DeclareLocal(statement.second_name, second_binding, value_type, statement.location,
                 &statement.second_name);
  }
  controls_.push_back(
      {.break_jumps = {}, .continue_jumps = {}, .finally_depth = finalizers_.size()});
  LowerScopedStatement(*statement.then_branch);
  for (const auto jump : controls_.back().continue_jumps) {
    current_function_->instructions.at(jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
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
  const auto loop_end = static_cast<std::uint32_t>(current_function_->instructions.size());
  current_function_->instructions.at(exit_jump).index = loop_end;
  for (const auto jump : controls_.back().break_jumps) {
    current_function_->instructions.at(jump).index = loop_end;
  }
  controls_.pop_back();
  PopScope();
}

}  // namespace vela::ir
