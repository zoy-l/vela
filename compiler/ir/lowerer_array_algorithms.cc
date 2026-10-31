#include "base/array_algorithm.h"
#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "base/source_location.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/frontend/type_names.h"
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
                                      const frontend::FunctionDescriptor& signature,
                                      std::span<const frontend::TypeDescriptor> argument_types,
                                      RegisterId guard,
                                      frontend::SourceLocation location) {
  const auto base = AllocateRegisterWindow(arguments.size(), location);
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    auto argument = LoweredValue{
        .reg = arguments.subspan(index).front(),
        .type = argument_types.subspan(index).front(),
    };
    RecordValueType(argument.reg, argument.type);
    const auto& target = signature.parameters.at(index).type;
    if (argument.type.kind == frontend::TypeDescriptor::Kind::Dynamic ||
        target.kind == frontend::TypeDescriptor::Kind::Dynamic) {
      argument = LowerConversion(argument, target, location);
    }
    const auto slot = base + static_cast<RegisterId>(index);
    Emit({.opcode = Opcode::Move, .dst = slot, .lhs = argument.reg, .location = location});
  }
  const auto result = EmitTemporary({
      .opcode = Opcode::CallIndirect,
      .lhs = base,
      .rhs = callback,
      .index = static_cast<std::uint32_t>(arguments.size()),
      .location = location,
  });
  RecordValueType(result, signature.return_type);
  // 同一个已求值的 receiver/version 调用窗口跨所有迭代复用，避免逐次物化检查参数。
  EmitBuiltinCall(Builtin::ArrayCheckMutation, AllocateTemporary(location), guard, location);
  return result;
}

Lowerer::LoweredValue Lowerer::LowerArrayAlgorithm(const frontend::Expression& expression) {
  const auto algorithm = model_.array_algorithms.at(&expression);
  const auto location = expression.location;
  // 所有输入先保存到帧槽；实参中的赋值不能改变已求值的 receiver 或其他实参。
  const auto slots = std::max<std::size_t>(expression.arguments.size() + 1, 3);
  const auto base = AllocateRegisterWindow(slots, location);
  const auto receiver = LowerExpression(*expression.callee->object);
  Emit({.opcode = Opcode::Move, .dst = base, .lhs = receiver.reg, .location = location});
  const auto& order = model_.argument_order.at(&expression);
  const frontend::FunctionDescriptor* signature = nullptr;
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    const auto value = LowerExpression(*expression.arguments.at(index).value);
    if (order.at(index) == 0) {
      signature = &model_.function_types.at(value.type.name);
    }
    Emit({
        .opcode = Opcode::Move,
        .dst = base + 1 + order.at(index),
        .lhs = value.reg,
        .location = location,
    });
  }
  if (algorithm == ArrayAlgorithm::Sort && signature == nullptr) {
    const auto result = AllocateTemporary(location);
    EmitBuiltinCall(Builtin::ArraySortDefault, result, base, location);
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }
  using Comparison = ArrayAlgorithmRegisters::Comparison;
  auto comparison = Comparison::Integer;
  std::size_t callback_arity = 0;
  if (signature != nullptr) {
    callback_arity = signature->parameters.size();
    comparison = signature->return_type.kind == frontend::TypeDescriptor::Kind::Double
                     ? Comparison::Double
                     : Comparison::Integer;
  }
  const auto length = AllocateTemporary(location);
  EmitBuiltinCall(Builtin::ArrayLength, length, base, location);
  const auto callback = base + 1;
  const auto guard = AllocateRegisterWindow(2, location);
  Emit({.opcode = Opcode::Move, .dst = guard, .lhs = base, .location = location});
  EmitBuiltinCall(Builtin::ArrayMutation, guard + 1, base, location);
  return LowerArrayTraversal(
      {
          .array = base,
          .callback = callback,
          .length = length,
          .guard = guard,
          .positions = 0,
          .callback_arity = callback_arity,
          .comparison = comparison,
          .element_type = model_.array_elements.at(receiver.type.name),
          .signature = signature,
      },
      algorithm, receiver.type, model_.expression_types.at(&expression), location);
}

Lowerer::LoweredValue Lowerer::LowerArraySortTraversal(const ArrayAlgorithmRegisters& inputs,
                                                       const frontend::TypeDescriptor& result_type,
                                                       frontend::SourceLocation location) {
  const auto base = inputs.array;
  const auto length = inputs.length;
  const auto arguments = AllocateRegisterWindow(3, location);
  Emit({.opcode = Opcode::Move, .dst = arguments, .lhs = base, .location = location});
  Emit({.opcode = Opcode::LoadInt, .dst = arguments + 1, .location = location});
  Emit({.opcode = Opcode::Move, .dst = arguments + 2, .lhs = length, .location = location});
  const auto work = AllocateTemporary(location);
  EmitBuiltinCall(Builtin::ArraySlice, work, arguments, location);
  RecordValueType(work, result_type);
  const auto positions = EmitTemporary({
      .opcode = Opcode::NewDynamicArray,
      .lhs = length,
      .index = RegisterArrayType(frontend::MakeArrayType(
          {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName})),
      .location = location,
  });
  const auto zero = EmitTemporary({.opcode = Opcode::LoadInt, .location = location});
  const auto one = EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 1, .location = location});
  const auto index = EmitTemporary({.opcode = Opcode::Move, .lhs = zero, .location = location});
  const auto initialize = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto within =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = index, .rhs = length, .location = location});
  const auto initialized = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = within, .location = location});
  Emit({
      .opcode = Opcode::SetIndex,
      .dst = positions,
      .lhs = index,
      .rhs = index,
      .location = location,
  });
  Emit({.opcode = Opcode::AddInt, .dst = index, .lhs = index, .rhs = one, .location = location});
  Emit({.opcode = Opcode::Jump, .index = initialize, .location = location});
  current_function_->instructions.at(initialized).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
  auto sorting = inputs;
  sorting.array = work;
  sorting.positions = positions;
  LowerArraySort(sorting, location);
  // 比较器读取快照；全部比较完成后写回原数组，等值元素按原始位置保持稳定。
  Emit({.opcode = Opcode::Move, .dst = index, .lhs = zero, .location = location});
  const auto write = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto more =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = index, .rhs = length, .location = location});
  const auto done = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = more, .location = location});
  const auto value =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = work, .rhs = index, .location = location});
  RecordValueType(value, inputs.element_type);
  Emit({
      .opcode = Opcode::SetIndex,
      .dst = base,
      .lhs = index,
      .rhs = value,
      .location = location,
  });
  Emit({.opcode = Opcode::AddInt, .dst = index, .lhs = index, .rhs = one, .location = location});
  Emit({.opcode = Opcode::Jump, .index = write, .location = location});
  current_function_->instructions.at(done).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
  return {
      .reg = base,
      .type = result_type,
  };
}

RegisterId Lowerer::CheckArrayCallbackResult(RegisterId value,
                                             const ArrayAlgorithmRegisters& inputs,
                                             ArrayAlgorithm algorithm,
                                             const frontend::TypeDescriptor& result_type,
                                             frontend::SourceLocation location) {
  if (inputs.validation_plan) {
    const std::array checked{*inputs.validation_plan, value};
    const auto validated = AllocateTemporary(location);
    if (algorithm != ArrayAlgorithm::Map && algorithm != ArrayAlgorithm::ForEach) {
      RecordValueType(validated, algorithm == ArrayAlgorithm::Reduce
                                     ? result_type
                                     : frontend::TypeDescriptor{
                                           .kind = frontend::TypeDescriptor::Kind::Bool,
                                           .name = frontend::kBoolTypeName,
                                       });
    }
    EmitBuiltinCall(Builtin::DynamicArrayValidate, validated, checked, location);
    value = validated;
  }
  const auto& callback_result = inputs.signature->return_type;
  const auto expected_result = algorithm == ArrayAlgorithm::Reduce
                                   ? result_type
                                   : frontend::TypeDescriptor{
                                         .kind = frontend::TypeDescriptor::Kind::Bool,
                                         .name = frontend::kBoolTypeName,
                                     };
  if (!inputs.validation_plan && callback_result.kind == frontend::TypeDescriptor::Kind::Dynamic &&
      algorithm != ArrayAlgorithm::Map && algorithm != ArrayAlgorithm::ForEach) {
    value = LowerConversion({.reg = value, .type = callback_result}, expected_result, location).reg;
  }
  if (algorithm == ArrayAlgorithm::Reduce &&
      expected_result.kind == frontend::TypeDescriptor::Kind::Dynamic &&
      callback_result.kind == frontend::TypeDescriptor::Kind::Enum) {
    value = LowerConversion({.reg = value, .type = callback_result}, expected_result, location).reg;
  }
  return value;
}

Lowerer::LoweredValue Lowerer::LowerArrayTraversal(const ArrayAlgorithmRegisters& inputs,
                                                   ArrayAlgorithm algorithm,
                                                   const frontend::TypeDescriptor& receiver_type,
                                                   const frontend::TypeDescriptor& result_type,
                                                   frontend::SourceLocation location,
                                                   std::optional<RegisterId> mapped_output) {
  using Algorithm = ArrayAlgorithm;
  const auto base = inputs.array;
  const auto callback = inputs.callback;
  const auto length = inputs.length;
  const auto guard = inputs.guard;
  const auto callback_arity = inputs.callback_arity;
  const auto* signature = inputs.signature;
  if (algorithm == Algorithm::Sort) {
    return LowerArraySortTraversal(inputs, result_type, location);
  }
  const auto zero =
      EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 0, .location = location});
  const auto one = EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 1, .location = location});
  const auto index = EmitTemporary({.opcode = Opcode::Move, .lhs = zero, .location = location});
  Instruction initial{.opcode = Opcode::LoadNull, .location = location};
  if (algorithm == Algorithm::Reduce) {
    initial.opcode = Opcode::Move;
    initial.lhs = base + 2;
  } else if (algorithm == Algorithm::Map) {
    if (mapped_output) {
      initial.opcode = Opcode::Move;
      initial.lhs = *mapped_output;
    } else {
      initial.opcode = Opcode::NewDynamicArray;
      initial.lhs = length;
      initial.index = RegisterArrayType(result_type);
    }
  } else if (algorithm == Algorithm::FindIndex) {
    initial.opcode = Opcode::LoadInt;
    initial.immediate = -1;
  } else if (algorithm == Algorithm::Some || algorithm == Algorithm::Every) {
    initial.opcode = Opcode::LoadBool;
    initial.immediate = algorithm == Algorithm::Every ? 1 : 0;
  }
  const auto result = EmitTemporary(initial);
  RecordValueType(result, result_type);
  const auto loop = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto condition =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = index, .rhs = length, .location = location});
  const auto exit = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition, .location = location});
  const auto element =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = base, .rhs = index, .location = location});
  RecordValueType(element, inputs.element_type);
  auto arguments = algorithm == Algorithm::Reduce ? std::vector{result, element, index, base}
                                                  : std::vector{element, index, base};
  arguments.resize(callback_arity);
  const auto& element_type = inputs.element_type;
  const frontend::TypeDescriptor integer{
      .kind = frontend::TypeDescriptor::Kind::Int,
      .name = frontend::kIntTypeName,
  };
  auto argument_types = algorithm == Algorithm::Reduce
                            ? std::vector{result_type, element_type, integer, receiver_type}
                            : std::vector{element_type, integer, receiver_type};
  auto value = EmitArrayCallback(callback, arguments, *signature, argument_types, guard, location);
  value = CheckArrayCallbackResult(value, inputs, algorithm, result_type, location);
  std::optional<std::size_t> short_exit;
  if (algorithm == Algorithm::Reduce) {
    Emit({.opcode = Opcode::Move, .dst = result, .lhs = value, .location = location});
  } else if (algorithm == Algorithm::Map) {
    const LoweredValue output{.reg = result, .type = result_type};
    EmitIndexWrite(output, index, value, location);
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
  return {.reg = result, .type = result_type};
}

RegisterId Lowerer::EmitArrayLess(const ArrayAlgorithmRegisters& inputs,
                                  std::pair<RegisterId, RegisterId> indices,
                                  frontend::SourceLocation location) {
  const auto [left, right] = indices;
  const auto left_value = EmitTemporary(
      {.opcode = Opcode::GetIndex, .lhs = inputs.array, .rhs = left, .location = location});
  const auto right_value = EmitTemporary(
      {.opcode = Opcode::GetIndex, .lhs = inputs.array, .rhs = right, .location = location});
  RecordValueType(left_value, inputs.element_type);
  RecordValueType(right_value, inputs.element_type);
  const auto values = std::array{left_value, right_value};
  const std::array types{inputs.element_type, inputs.element_type};
  auto comparison =
      EmitArrayCallback(inputs.callback, std::span(values).first(inputs.callback_arity),
                        *inputs.signature, types, inputs.guard, location);
  if (inputs.validation_plan) {
    const std::array arguments{*inputs.validation_plan, comparison};
    const auto result = AllocateTemporary(location);
    RecordValueType(result,
                    {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
    EmitBuiltinCall(Builtin::DynamicArrayValidate, result, arguments, location);
    comparison = result;
  }
  using Comparison = ArrayAlgorithmRegisters::Comparison;
  auto zero = EmitTemporary({.opcode = Opcode::LoadInt, .location = location});
  const bool floating = inputs.comparison == Comparison::Double;
  if (floating) {
    frontend::Expression literal;
    literal.location = location;
    zero = LowerDoubleLiteral(literal).reg;
  }
  const auto less = EmitTemporary({
      .opcode = floating ? Opcode::LessDouble : Opcode::LessInt,
      .lhs = comparison,
      .rhs = zero,
      .location = location,
  });
  const auto greater = EmitTemporary({
      .opcode = floating ? Opcode::GreaterDouble : Opcode::GreaterInt,
      .lhs = comparison,
      .rhs = zero,
      .location = location,
  });
  const auto tie = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = less, .location = location});
  const auto less_done = current_function_->instructions.size();
  Emit({.opcode = Opcode::Jump, .location = location});
  current_function_->instructions.at(tie).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto equal = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = greater, .location = location});
  const auto greater_done = current_function_->instructions.size();
  Emit({.opcode = Opcode::Jump, .location = location});
  current_function_->instructions.at(equal).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto left_position = EmitTemporary(
      {.opcode = Opcode::GetIndex, .lhs = inputs.positions, .rhs = left, .location = location});
  const auto right_position = EmitTemporary(
      {.opcode = Opcode::GetIndex, .lhs = inputs.positions, .rhs = right, .location = location});
  RecordValueType(left_position,
                  {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
  RecordValueType(right_position,
                  {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
  Emit({
      .opcode = Opcode::LessInt,
      .dst = less,
      .lhs = left_position,
      .rhs = right_position,
      .location = location,
  });
  const auto end = static_cast<std::uint32_t>(current_function_->instructions.size());
  current_function_->instructions.at(less_done).index = end;
  current_function_->instructions.at(greater_done).index = end;
  return less;
}

void Lowerer::LowerArraySiftDown(RegisterId root,
                                 const ArrayAlgorithmRegisters& inputs,
                                 frontend::SourceLocation location) {
  const auto array = inputs.array;
  const auto length = inputs.length;
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
  const auto less = EmitArrayLess(inputs, {child, right}, location);
  const auto keep_left = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = less, .location = location});
  Emit({.opcode = Opcode::Move, .dst = child, .lhs = right, .location = location});
  const auto selected = static_cast<std::uint32_t>(current_function_->instructions.size());
  current_function_->instructions.at(no_right).index = selected;
  current_function_->instructions.at(keep_left).index = selected;
  const auto needs_swap = EmitArrayLess(inputs, {root, child}, location);
  const auto root_value =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = root, .location = location});
  const auto child_value =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = child, .location = location});
  RecordValueType(root_value, inputs.element_type);
  RecordValueType(child_value, inputs.element_type);
  const auto ordered = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = needs_swap, .location = location});
  const auto root_position = EmitTemporary(
      {.opcode = Opcode::GetIndex, .lhs = inputs.positions, .rhs = root, .location = location});
  const auto child_position = EmitTemporary(
      {.opcode = Opcode::GetIndex, .lhs = inputs.positions, .rhs = child, .location = location});
  RecordValueType(root_position,
                  {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
  RecordValueType(child_position,
                  {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
  Emit({
      .opcode = Opcode::SetIndex,
      .dst = inputs.positions,
      .lhs = root,
      .rhs = child_position,
      .location = location,
  });
  Emit({
      .opcode = Opcode::SetIndex,
      .dst = inputs.positions,
      .lhs = child,
      .rhs = root_position,
      .location = location,
  });
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
  const auto array = inputs.array;
  const auto length = inputs.length;
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
  const auto first_position = EmitTemporary(
      {.opcode = Opcode::GetIndex, .lhs = inputs.positions, .rhs = zero, .location = location});
  const auto last_position = EmitTemporary(
      {.opcode = Opcode::GetIndex, .lhs = inputs.positions, .rhs = end, .location = location});
  RecordValueType(first_position,
                  {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
  RecordValueType(last_position,
                  {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
  Emit({
      .opcode = Opcode::SetIndex,
      .dst = inputs.positions,
      .lhs = zero,
      .rhs = last_position,
      .location = location,
  });
  Emit({
      .opcode = Opcode::SetIndex,
      .dst = inputs.positions,
      .lhs = end,
      .rhs = first_position,
      .location = location,
  });
  const auto first =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = zero, .location = location});
  const auto last =
      EmitTemporary({.opcode = Opcode::GetIndex, .lhs = array, .rhs = end, .location = location});
  RecordValueType(first, inputs.element_type);
  RecordValueType(last, inputs.element_type);
  Emit({.opcode = Opcode::SetIndex, .dst = array, .lhs = zero, .rhs = last, .location = location});
  Emit({.opcode = Opcode::SetIndex, .dst = array, .lhs = end, .rhs = first, .location = location});
  const auto start = EmitTemporary({.opcode = Opcode::Move, .lhs = zero, .location = location});
  auto remaining_inputs = inputs;
  remaining_inputs.length = end;
  LowerArraySiftDown(start, remaining_inputs, location);
  Emit({.opcode = Opcode::SubInt, .dst = end, .lhs = end, .rhs = one, .location = location});
  Emit({.opcode = Opcode::Jump, .index = sort, .location = location});
  current_function_->instructions.at(done).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
}

}  // namespace vela::ir
