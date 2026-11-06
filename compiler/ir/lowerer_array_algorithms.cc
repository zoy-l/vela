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
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

RegisterId Lowerer::EmitTemporary(Instruction instruction) {
  const auto result = AllocateRegister(instruction.location);
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
        .reg = arguments[index],
        .type = argument_types[index],
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
  EmitBuiltinCall(Builtin::ArrayCheckMutation, AllocateRegister(location), guard, location);
  return result;
}

Lowerer::LoweredValue Lowerer::LowerArrayAlgorithm(const frontend::Expression& expression) {
  const auto& result_type = model_.expression_types.at(&expression);
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
    const auto result = AllocateRegister(location);
    EmitBuiltinCall(Builtin::ArraySortDefault, result, base, location);
    return {.reg = result, .type = result_type};
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
  const auto length = AllocateRegister(location);
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
          .callback_arity = callback_arity,
          .comparison = comparison,
          .element_type = model_.array_elements.at(receiver.type.name),
          .signature = signature,
      },
      algorithm, receiver.type, result_type, location);
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
  const auto work = AllocateRegister(location);
  EmitBuiltinCall(Builtin::ArraySlice, work, arguments, location);
  RecordValueType(work, result_type);
  const auto scratch = EmitTemporary({
      .opcode = Opcode::NewDynamicArray,
      .lhs = length,
      .index = RegisterArrayType(result_type),
      .location = location,
  });
  RecordValueType(scratch, result_type);
  const auto zero = EmitTemporary({.opcode = Opcode::LoadInt, .location = location});
  const auto one = EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 1, .location = location});
  const auto index = EmitTemporary({.opcode = Opcode::Move, .lhs = zero, .location = location});
  auto sorting = inputs;
  sorting.array = work;
  LowerArraySort(sorting, scratch, location);
  // 比较器读取快照；全部比较成功后才写回原数组。
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
  BindJump(done);
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
    const auto validated = AllocateRegister(location);
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
    BindJump(skip_jump);
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
    const auto result = AllocateRegister(location);
    RecordValueType(result,
                    {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
    EmitBuiltinCall(Builtin::DynamicArrayValidate, result, arguments, location);
    comparison = result;
  }
  using Comparison = ArrayAlgorithmRegisters::Comparison;
  auto zero = EmitTemporary({.opcode = Opcode::LoadInt, .location = location});
  const bool floating = inputs.comparison == Comparison::Double;
  if (floating) {
    zero = EmitDoubleConstant(0, location).reg;
  }
  const auto less = EmitTemporary({
      .opcode = floating ? Opcode::LessDouble : Opcode::LessInt,
      .lhs = comparison,
      .rhs = zero,
      .location = location,
  });
  return less;
}

void Lowerer::LowerArrayMergePass(RegisterId output,
                                  const ArrayAlgorithmRegisters& inputs,
                                  RegisterId width,
                                  frontend::SourceLocation location) {
  const auto length = inputs.length;
  const auto one = EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 1, .location = location});
  const auto begin = EmitTemporary({.opcode = Opcode::LoadInt, .location = location});
  const auto pair = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto more =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = begin, .rhs = length, .location = location});
  const auto done = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = more, .location = location});
  const auto boundary = [&](RegisterId start) {
    const auto end =
        EmitTemporary({.opcode = Opcode::AddInt, .lhs = start, .rhs = width, .location = location});
    const auto exceeds = EmitTemporary(
        {.opcode = Opcode::GreaterInt, .lhs = end, .rhs = length, .location = location});
    const auto bounded = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfFalse, .lhs = exceeds, .location = location});
    Emit({.opcode = Opcode::Move, .dst = end, .lhs = length, .location = location});
    BindJump(bounded);
    return end;
  };
  const auto middle = boundary(begin);
  const auto end = boundary(middle);
  const auto left = EmitTemporary({.opcode = Opcode::Move, .lhs = begin, .location = location});
  const auto right = EmitTemporary({.opcode = Opcode::Move, .lhs = middle, .location = location});
  const auto destination =
      EmitTemporary({.opcode = Opcode::Move, .lhs = begin, .location = location});
  const auto merge = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto remaining = EmitTemporary(
      {.opcode = Opcode::LessInt, .lhs = destination, .rhs = end, .location = location});
  const auto merged = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = remaining, .location = location});
  const auto has_left =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = left, .rhs = middle, .location = location});
  const auto exhausted_left = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = has_left, .location = location});
  const auto has_right =
      EmitTemporary({.opcode = Opcode::LessInt, .lhs = right, .rhs = end, .location = location});
  const auto exhausted_right = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = has_right, .location = location});
  // 右值严格小于左值时才取右侧；相等或 NaN 都先取左侧，天然保持稳定性。
  const auto right_first = EmitArrayLess(inputs, {right, left}, location);
  const auto ordered_left = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = right_first, .location = location});
  BindJump(exhausted_left);
  const auto value = EmitTemporary(
      {.opcode = Opcode::GetIndex, .lhs = inputs.array, .rhs = right, .location = location});
  RecordValueType(value, inputs.element_type);
  Emit({.opcode = Opcode::AddInt, .dst = right, .lhs = right, .rhs = one, .location = location});
  const auto selected_right = current_function_->instructions.size();
  Emit({.opcode = Opcode::Jump, .location = location});
  const auto take_left = static_cast<std::uint32_t>(current_function_->instructions.size());
  current_function_->instructions.at(exhausted_right).index = take_left;
  current_function_->instructions.at(ordered_left).index = take_left;
  Emit({
      .opcode = Opcode::GetIndex,
      .dst = value,
      .lhs = inputs.array,
      .rhs = left,
      .location = location,
  });
  Emit({.opcode = Opcode::AddInt, .dst = left, .lhs = left, .rhs = one, .location = location});
  BindJump(selected_right);
  Emit({
      .opcode = Opcode::SetIndex,
      .dst = output,
      .lhs = destination,
      .rhs = value,
      .location = location,
  });
  Emit({
      .opcode = Opcode::AddInt,
      .dst = destination,
      .lhs = destination,
      .rhs = one,
      .location = location,
  });
  Emit({.opcode = Opcode::Jump, .index = merge, .location = location});
  BindJump(merged);
  Emit({.opcode = Opcode::Move, .dst = begin, .lhs = end, .location = location});
  Emit({.opcode = Opcode::Jump, .index = pair, .location = location});
  BindJump(done);
}

void Lowerer::LowerArraySort(const ArrayAlgorithmRegisters& inputs,
                             RegisterId scratch,
                             frontend::SourceLocation location) {
  const auto array = inputs.array;
  const auto width =
      EmitTemporary({.opcode = Opcode::LoadInt, .immediate = 1, .location = location});
  const auto loop = static_cast<std::uint32_t>(current_function_->instructions.size());
  const auto more = EmitTemporary(
      {.opcode = Opcode::LessInt, .lhs = width, .rhs = inputs.length, .location = location});
  const auto done = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = more, .location = location});
  LowerArrayMergePass(scratch, inputs, width, location);
  const auto previous = EmitTemporary({.opcode = Opcode::Move, .lhs = array, .location = location});
  Emit({.opcode = Opcode::Move, .dst = array, .lhs = scratch, .location = location});
  Emit({.opcode = Opcode::Move, .dst = scratch, .lhs = previous, .location = location});
  // 数组长度最多 uint32；最后一轮的加倍仍在 int64 范围内。
  Emit({.opcode = Opcode::AddInt, .dst = width, .lhs = width, .rhs = width, .location = location});
  Emit({.opcode = Opcode::Jump, .index = loop, .location = location});
  BindJump(done);
}

}  // namespace vela::ir
