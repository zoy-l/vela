#include "compiler/ir/lowerer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "base/builtin.h"
#include "base/source_location.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

RegisterId Lowerer::EmitTemporary(Instruction instruction) {
  const auto result = AllocateTemporary(instruction.location);
  instruction.dst = result;
  Emit(instruction);
  return result;
}

RegisterId Lowerer::EmitArrayCallback(RegisterId callback,
                                      std::span<const RegisterId> arguments,
                                      RegisterId guard,
                                      frontend::SourceLocation location) {
  const auto base = next_register_;
  for (const auto argument : arguments) {
    const auto slot = AllocateRegister(location);
    Emit({.opcode = Opcode::Move, .dst = slot, .lhs = argument, .location = location});
  }
  const auto result = EmitTemporary({
      .opcode = Opcode::CallIndirect,
      .lhs = base,
      .rhs = callback,
      .index = static_cast<std::uint32_t>(arguments.size()),
      .location = location,
  });
  // 同一个已求值的 receiver/version 调用窗口跨所有迭代复用，避免逐次物化检查参数。
  static_cast<void>(EmitTemporary({
      .opcode = Opcode::BuiltinCall,
      .lhs = guard,
      .index = static_cast<std::uint32_t>(Builtin::ArrayCheckMutation),
      .location = location,
  }));
  return result;
}

Lowerer::LoweredValue Lowerer::LowerArrayAlgorithm(const frontend::Expression& expression) {
  using Algorithm = frontend::ArrayAlgorithm;
  const auto algorithm = model_.array_algorithms.at(&expression);
  const auto location = expression.location;
  const auto base = next_register_;
  // 所有输入先保存到帧槽；实参中的赋值不能改变已求值的 receiver 或其他实参。
  for (std::size_t index = 0; index <= expression.arguments.size(); ++index) {
    static_cast<void>(AllocateRegister(location));
  }
  const auto receiver = LowerExpression(*expression.callee->object);
  Emit({.opcode = Opcode::Move, .dst = base, .lhs = receiver.reg, .location = location});
  const auto& order = model_.argument_order.at(&expression);
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    const auto value = LowerExpression(*expression.arguments.at(index).value);
    Emit({
        .opcode = Opcode::Move,
        .dst = base + 1 + order.at(index),
        .lhs = value.reg,
        .location = location,
    });
  }
  const auto length = EmitTemporary({
      .opcode = Opcode::BuiltinCall,
      .lhs = base,
      .index = static_cast<std::uint32_t>(Builtin::ArrayLength),
      .location = location,
  });
  const auto callback = base + (algorithm == Algorithm::Reduce ? 2 : 1);
  const auto guard = next_register_;
  static_cast<void>(AllocateRegister(location));
  static_cast<void>(AllocateRegister(location));
  Emit({.opcode = Opcode::Move, .dst = guard, .lhs = base, .location = location});
  Emit({
      .opcode = Opcode::BuiltinCall,
      .dst = guard + 1,
      .lhs = base,
      .index = static_cast<std::uint32_t>(Builtin::ArrayMutation),
      .location = location,
  });
  if (algorithm == Algorithm::Sort) {
    LowerArraySort({.array = base, .callback = callback, .length = length, .guard = guard},
                   location);
    return {
        .reg = EmitTemporary({.opcode = Opcode::LoadNull, .location = location}),
        .type = model_.expression_types.at(&expression),
    };
  }
  const auto zero =
      EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 0, .location = location});
  const auto one = EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 1, .location = location});
  const auto index = EmitTemporary({.opcode = Opcode::Move, .lhs = zero, .location = location});
  Instruction initial{.opcode = Opcode::LoadNull, .location = location};
  if (algorithm == Algorithm::Reduce) {
    initial.opcode = Opcode::Move;
    initial.lhs = base + 1;
  } else if (algorithm == Algorithm::Map) {
    initial.opcode = Opcode::NewDynamicArray;
    initial.lhs = length;
    initial.index = RegisterArrayType(model_.expression_types.at(&expression));
  } else if (algorithm == Algorithm::FindIndex) {
    initial.opcode = Opcode::LoadInt;
    initial.immediate = -1;
  } else if (algorithm == Algorithm::Some || algorithm == Algorithm::Every) {
    initial.opcode = Opcode::LoadBool;
    initial.immediate = algorithm == Algorithm::Every ? 1 : 0;
  }
  const auto result = EmitTemporary(initial);
  const auto loop = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto condition =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = index, .rhs = length, .location = location});
  const auto exit = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition, .location = location});
  const auto element =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = base, .rhs = index, .location = location});
  const auto arguments =
      algorithm == Algorithm::Reduce ? std::vector{result, element} : std::vector{element};
  const auto value = EmitArrayCallback(callback, arguments, guard, location);
  std::optional<std::size_t> short_exit;
  if (algorithm == Algorithm::Reduce) {
    Emit({.opcode = Opcode::Move, .dst = result, .lhs = value, .location = location});
  } else if (algorithm == Algorithm::Map) {
    Emit({
        .opcode = Opcode::SetIndex,
        .dst = result,
        .lhs = index,
        .rhs = value,
        .location = location,
    });
  } else if (algorithm != Algorithm::ForEach) {
    const auto skip_condition =
        algorithm == Algorithm::Every
            ? EmitTemporary({.opcode = Opcode::NotBool, .lhs = value, .location = location})
            : value;
    // NotBool 可能发出一条指令，因此跳转下标在其后记录。
    const auto skip_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfFalse, .lhs = skip_condition, .location = location});
    Emit({
        .opcode = Opcode::Move,
        .dst = result,
        .lhs = algorithm == Algorithm::FindIndex ? index : value,
        .location = location,
    });
    short_exit = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = location});
    current_function_->instructions.at(skip_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
  Emit({.opcode = Opcode::AddInt, .dst = index, .lhs = index, .rhs = one, .location = location});
  Emit({.opcode = Opcode::Jump, .index = loop, .location = location});
  const auto end = static_cast<std::uint32_t>(current_function_->instructions.size());
  current_function_->instructions.at(exit).index = end;
  if (short_exit) {
    current_function_->instructions.at(*short_exit).index = end;
  }
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

void Lowerer::LowerArraySiftDown(RegisterId root,
                                 const ArrayAlgorithmRegisters& inputs,
                                 frontend::SourceLocation location) {
  const auto [array, callback, length, guard] = inputs;
  const auto zero = EmitTemporary({.opcode = Opcode::LoadInt, .location = location});
  const auto one = EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 1, .location = location});
  const auto two = EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 2, .location = location});
  const auto half =
      EmitTemporary({.opcode = Opcode::DivInt, .lhs = length, .rhs = two, .location = location});
  const auto loop = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto within =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = root, .rhs = half, .location = location});
  const auto exit = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = within, .location = location});
  const auto child =
      EmitTemporary({.opcode = Opcode::MulInt, .lhs = root, .rhs = two, .location = location});
  Emit({.opcode = Opcode::AddInt, .dst = child, .lhs = child, .rhs = one, .location = location});
  const auto right =
      EmitTemporary({.opcode = Opcode::AddInt, .lhs = child, .rhs = one, .location = location});
  const auto has_right =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = right, .rhs = length, .location = location});
  const auto no_right = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = has_right, .location = location});
  const auto left_value =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = child, .location = location});
  const auto right_value =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = right, .location = location});
  const auto comparison =
      EmitArrayCallback(callback, std::array{left_value, right_value}, guard, location);
  const auto less = EmitTemporary(
      {.opcode = Opcode::LessInt, .lhs = comparison, .rhs = zero, .location = location});
  const auto keep_left = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = less, .location = location});
  Emit({.opcode = Opcode::Move, .dst = child, .lhs = right, .location = location});
  const auto selected = static_cast<std::uint32_t>(current_function_->instructions.size());
  current_function_->instructions.at(no_right).index = selected;
  current_function_->instructions.at(keep_left).index = selected;
  const auto root_value =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = root, .location = location});
  const auto child_value =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = child, .location = location});
  const auto order =
      EmitArrayCallback(callback, std::array{root_value, child_value}, guard, location);
  const auto needs_swap =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = order, .rhs = zero, .location = location});
  const auto ordered = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = needs_swap, .location = location});
  Emit({
      .opcode = Opcode::SetIndex,
      .dst = array,
      .lhs = root,
      .rhs = child_value,
      .location = location,
  });
  Emit({
      .opcode = Opcode::SetIndex,
      .dst = array,
      .lhs = child,
      .rhs = root_value,
      .location = location,
  });
  Emit({.opcode = Opcode::Move, .dst = root, .lhs = child, .location = location});
  Emit({.opcode = Opcode::Jump, .index = loop, .location = location});
  const auto end = static_cast<std::uint32_t>(current_function_->instructions.size());
  current_function_->instructions.at(exit).index = end;
  current_function_->instructions.at(ordered).index = end;
}

void Lowerer::LowerArraySort(const ArrayAlgorithmRegisters& inputs,
                             frontend::SourceLocation location) {
  const auto [array, callback, length, guard] = inputs;
  const auto zero = EmitTemporary({.opcode = Opcode::LoadInt, .location = location});
  const auto one = EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 1, .location = location});
  const auto two = EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 2, .location = location});
  const auto root =
      EmitTemporary({.opcode = Opcode::DivInt, .lhs = length, .rhs = two, .location = location});
  const auto build = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto remaining =
      EmitTemporary({.opcode = Opcode::GreaterInt, .lhs = root, .rhs = zero, .location = location});
  const auto built = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = remaining, .location = location});
  Emit({.opcode = Opcode::SubInt, .dst = root, .lhs = root, .rhs = one, .location = location});
  const auto sift_root = EmitTemporary({.opcode = Opcode::Move, .lhs = root, .location = location});
  LowerArraySiftDown(sift_root, inputs, location);
  Emit({.opcode = Opcode::Jump, .index = build, .location = location});
  current_function_->instructions.at(built).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto end =
      EmitTemporary({.opcode = Opcode::SubInt, .lhs = length, .rhs = one, .location = location});
  const auto sort = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto more =
      EmitTemporary({.opcode = Opcode::GreaterInt, .lhs = end, .rhs = zero, .location = location});
  const auto done = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = more, .location = location});
  const auto first =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = zero, .location = location});
  const auto last =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = end, .location = location});
  Emit({.opcode = Opcode::SetIndex, .dst = array, .lhs = zero, .rhs = last, .location = location});
  Emit({.opcode = Opcode::SetIndex, .dst = array, .lhs = end, .rhs = first, .location = location});
  const auto start = EmitTemporary({.opcode = Opcode::Move, .lhs = zero, .location = location});
  LowerArraySiftDown(start, {.array = array, .callback = callback, .length = end, .guard = guard},
                     location);
  Emit({.opcode = Opcode::SubInt, .dst = end, .lhs = end, .rhs = one, .location = location});
  Emit({.opcode = Opcode::Jump, .index = sort, .location = location});
  current_function_->instructions.at(done).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
}

}  // namespace vela::ir
