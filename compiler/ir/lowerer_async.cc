#include "compiler/frontend/ast.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "base/async_operation.h"

namespace vela::ir {

Lowerer::LoweredValue Lowerer::LowerAsyncCall(const frontend::Expression& expression) {
  const auto& descriptor = model_.async_calls.at(&expression);
  const auto operation = descriptor.operation;
  const auto count = AsyncArity(operation);
  std::optional<LoweredValue> receiver;
  const auto& member =
      expression.kind == frontend::Expression::Kind::Call ? *expression.callee : expression;
  if (descriptor.receiver) {
    receiver = LowerExpression(*member.object);
  }
  std::optional<std::size_t> null_jump;
  if (receiver && member.null_safe) {
    null_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfNull, .lhs = receiver->reg, .location = expression.location});
  }
  const auto base = next_register_;
  for (std::size_t index = 0; index < count; ++index) {
    const auto reg = AllocateRegister(expression.location);
    Emit({.opcode = Opcode::LoadNull, .dst = reg, .location = expression.location});
  }
  const auto hidden = receiver ? 1U : 0U;
  if (receiver) {
    Emit({
        .opcode = Opcode::Move,
        .dst = base,
        .lhs = receiver->reg,
        .location = expression.location,
    });
  }
  if (operation == AsyncOperation::Wait) {
    Emit({
        .opcode = Opcode::LoadBool,
        .dst = base + 1,
        .immediate = 0,
        .location = expression.location,
    });
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = base + 3,
        .immediate = -1,
        .location = expression.location,
    });
  }
  if (expression.kind == frontend::Expression::Kind::Call) {
    const auto& order = model_.argument_order.at(&expression);
    for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
      const auto& argument = expression.arguments.at(index);
      const auto value = LowerExpression(*argument.value);
      Emit({
          .opcode = Opcode::Move,
          .dst = base + hidden + order.at(index),
          .lhs = value.reg,
          .location = argument.location,
      });
    }
  }
  if (descriptor.runtime_type) {
    const auto type_index = operation == AsyncOperation::Wait
                                ? RegisterArrayType(*descriptor.runtime_type)
                                : RegisterRuntimeType(*descriptor.runtime_type);
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = base + static_cast<RegisterId>(count - 1),
        .immediate = type_index,
        .location = expression.location,
    });
  }
  const auto result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::AsyncCall,
      .dst = result,
      .lhs = base,
      .index = static_cast<std::uint32_t>(operation),
      .location = expression.location,
  });
  if (null_jump) {
    const auto end_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = expression.location});
    current_function_->instructions.at(*null_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    Emit({.opcode = Opcode::LoadNull, .dst = result, .location = expression.location});
    current_function_->instructions.at(end_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

}  // namespace vela::ir
