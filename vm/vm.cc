#include "vm/vm.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include "base/builtin.h"
#include "base/callable.h"
#include "base/execution_limits.h"
#include "vm/debugger.h"
#include "vm/execution.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/register_file.h"

#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/register_storage.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "runtime/value_io.h"
#include "vm/bytecode.h"
#include "vm/frame.h"

namespace vela::vm {
namespace internal {
namespace {

struct BinaryOperands final {
  runtime::RegisterReference left;
  runtime::RegisterReference right;
};

bool IsEqualValue(runtime::Value left, runtime::Value right) noexcept {
  if (left.kind() != right.kind()) {
    return false;
  }
  switch (left.kind()) {
    case runtime::ValueKind::Null:
      return true;
    case runtime::ValueKind::Boolean:
      return left.AsBoolean() == right.AsBoolean();
    case runtime::ValueKind::Integer:
      return left.AsInteger() == right.AsInteger();
    case runtime::ValueKind::Double:
      return left.AsDouble() == right.AsDouble();
    case runtime::ValueKind::String:
      return left.AsString()->value() == right.AsString()->value();
    case runtime::ValueKind::Object:
      return left.AsObject() == right.AsObject();
  }
  return false;
}

[[clang::always_inline]] StepResult IntegerArithmetic(OpCode opcode,
                                                      runtime::RegisterReference destination,
                                                      std::int64_t left,
                                                      std::int64_t right) noexcept {
  std::int64_t result = 0;
  bool overflow = false;
  switch (opcode) {
    case OpCode::AddInt:
      overflow = __builtin_add_overflow(left, right, &result);
      break;
    case OpCode::SubInt:
      overflow = __builtin_sub_overflow(left, right, &result);
      break;
    case OpCode::MulInt:
      overflow = __builtin_mul_overflow(left, right, &result);
      break;
    case OpCode::DivInt:
      if (right == 0) {
        return std::unexpected(ExecutionStatus::DivisionByZero);
      }
      overflow = left == std::numeric_limits<std::int64_t>::min() && right == -1;
      if (!overflow) {
        result = left / right;
      }
      break;
    default:
      std::unreachable();
  }
  if (overflow) {
    return std::unexpected(ExecutionStatus::IntegerOverflow);
  }
  destination = runtime::Value::Integer(result);
  return {};
}

[[clang::always_inline]] StepResult IntegerOperation(OpCode opcode,
                                                     runtime::RegisterReference destination,
                                                     BinaryOperands operands) noexcept {
  const auto& [left_value, right_value] = operands;
  if (!left_value.IsInteger() || !right_value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto left = left_value.AsInteger();
  const auto right = right_value.AsInteger();
  bool result = false;
  switch (opcode) {
    case OpCode::EqualInt:
      result = left == right;
      break;
    case OpCode::NotEqualInt:
      result = left != right;
      break;
    case OpCode::LessInt:
      result = left < right;
      break;
    case OpCode::LessEqualInt:
      result = left <= right;
      break;
    case OpCode::GreaterInt:
      result = left > right;
      break;
    case OpCode::GreaterEqualInt:
      result = left >= right;
      break;
    default:
      return IntegerArithmetic(opcode, destination, left, right);
  }
  destination = runtime::Value::Boolean(result);
  return {};
}

[[clang::always_inline]] StepResult DoubleOperation(OpCode opcode,
                                                    runtime::RegisterReference destination,
                                                    BinaryOperands operands) noexcept {
  const auto& [left_value, right_value] = operands;
  if (!left_value.IsDouble() || !right_value.IsDouble()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const double left = left_value.AsDouble();
  const double right = right_value.AsDouble();
  switch (opcode) {
    case OpCode::AddDouble:
      destination = runtime::Value::Double(left + right);
      break;
    case OpCode::SubDouble:
      destination = runtime::Value::Double(left - right);
      break;
    case OpCode::MulDouble:
      destination = runtime::Value::Double(left * right);
      break;
    case OpCode::DivDouble:
      if (right == 0.0) {
        return std::unexpected(ExecutionStatus::DivisionByZero);
      }
      destination = runtime::Value::Double(left / right);
      break;
    case OpCode::EqualDouble:
      destination = runtime::Value::Boolean(left == right);
      break;
    case OpCode::NotEqualDouble:
      destination = runtime::Value::Boolean(left != right);
      break;
    case OpCode::LessDouble:
      destination = runtime::Value::Boolean(left < right);
      break;
    case OpCode::LessEqualDouble:
      destination = runtime::Value::Boolean(left <= right);
      break;
    case OpCode::GreaterDouble:
      destination = runtime::Value::Boolean(left > right);
      break;
    case OpCode::GreaterEqualDouble:
      destination = runtime::Value::Boolean(left >= right);
      break;
    default:
      std::unreachable();
  }
  return {};
}

[[clang::always_inline]] StepResult BooleanOperation(OpCode opcode,
                                                     runtime::RegisterReference destination,
                                                     BinaryOperands operands) noexcept {
  const auto& [left_value, right_value] = operands;
  if (!left_value.IsBoolean() || !right_value.IsBoolean()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const bool left = left_value.AsBoolean();
  const bool right = right_value.AsBoolean();
  bool result = false;
  switch (opcode) {
    case OpCode::EqualBool:
      result = left == right;
      break;
    case OpCode::NotEqualBool:
      result = left != right;
      break;
    case OpCode::AndBool:
      result = left && right;
      break;
    case OpCode::OrBool:
      result = left || right;
      break;
    default:
      std::unreachable();
  }
  destination = runtime::Value::Boolean(result);
  return {};
}

}  // namespace

template <bool Debugging>
ExecutionResult Execution::RunImpl(std::uint32_t entry_function, Debugger* debugger) noexcept {
  auto& entry = frames_.front();
  entry.function = &functions_.subspan(entry_function).front();
  auto entry_values = registers_.front().Acquire(entry.function->register_count);
  if (entry_values.IsEmpty()) {
    return {
        .value = runtime::Value::Null(),
        .status = ExecutionStatus::OutOfMemory,
        .stack_trace = {},
        .message = {},
    };
  }
  entry.frame.Attach(isolate_.heap(), entry_values);
  frame_count_ = 1;
  while (frame_count_ != 0) {
    if constexpr (Debugging) {
      const DebugContext context(frames_.data(), frame_count_);
      if (debugger->BeforeInstruction(context) == DebugAction::Stop) {
        AbortInitializers();
        return {
            .value = runtime::Value::Null(),
            .status = ExecutionStatus::DebuggerStopped,
            .stack_trace = {},
            .message = {},
        };
      }
    }
    auto& active = std::span(frames_).subspan(frame_count_ - 1).front();
    const auto code = std::span(active.function->code);
    const auto instruction = code.subspan(active.program_counter++).front();
    const auto step = ExecuteInstruction(active, instruction);
    if (!step) {
      if (auto failure = HandleError(step.error())) {
        return std::move(*failure);
      }
    }
  }
  return {.value = result_, .status = ExecutionStatus::Returned, .stack_trace = {}, .message = {}};
}

ExecutionResult Execution::Run(std::uint32_t entry_function) noexcept {
  return RunImpl<false>(entry_function, nullptr);
}

ExecutionResult Execution::DebugRun(std::uint32_t entry_function, Debugger& debugger) noexcept {
  return RunImpl<true>(entry_function, &debugger);
}

StepResult Execution::ExecuteInstruction(ActiveFrame& active, Instruction instruction) {
  auto values = active.frame.registers();
  const auto opcode = DecodeOp(instruction);
  const auto operand_a = DecodeA(instruction);
  const auto operand_b = DecodeB(instruction);
  const auto operand_c = DecodeC(instruction);
  switch (opcode) {
    case OpCode::LoadNull:
      values.subspan(operand_a).front() = runtime::Value::Null();
      return {};
    case OpCode::LoadBool:
      values.subspan(operand_a).front() = runtime::Value::Boolean(operand_b != 0);
      return {};
    case OpCode::LoadInt:
      values.subspan(operand_a).front() = runtime::Value::Integer(DecodeImm16(instruction));
      return {};
    case OpCode::LoadIntConstant:
      values.subspan(operand_a).front() = runtime::Value::Integer(
          std::span(active.function->integers).subspan(DecodeUnsigned16(instruction)).front());
      return {};
    case OpCode::LoadDouble:
      values.subspan(operand_a).front() = runtime::Value::Double(
          std::span(active.function->doubles).subspan(DecodeUnsigned16(instruction)).front());
      return {};
    case OpCode::AddDouble:
    case OpCode::SubDouble:
    case OpCode::MulDouble:
    case OpCode::DivDouble:
    case OpCode::EqualDouble:
    case OpCode::NotEqualDouble:
    case OpCode::LessDouble:
    case OpCode::LessEqualDouble:
    case OpCode::GreaterDouble:
    case OpCode::GreaterEqualDouble:
      return DoubleOperation(
          opcode, values.subspan(operand_a).front(),
          {.left = values.subspan(operand_b).front(), .right = values.subspan(operand_c).front()});
    case OpCode::NegDouble: {
      const auto operand = values.subspan(operand_b).front();
      if (!operand.IsDouble()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      values.subspan(operand_a).front() = runtime::Value::Double(-operand.AsDouble());
      return {};
    }
    case OpCode::Move:
      values.subspan(operand_a).front() = values.subspan(operand_b).front();
      return {};
    case OpCode::AddInt:
    case OpCode::SubInt:
    case OpCode::MulInt:
    case OpCode::DivInt:
    case OpCode::EqualInt:
    case OpCode::NotEqualInt:
    case OpCode::LessInt:
    case OpCode::LessEqualInt:
    case OpCode::GreaterInt:
    case OpCode::GreaterEqualInt:
      return IntegerOperation(
          opcode, values.subspan(operand_a).front(),
          {.left = values.subspan(operand_b).front(), .right = values.subspan(operand_c).front()});
    case OpCode::NegInt: {
      const auto operand = values.subspan(operand_b).front();
      if (!operand.IsInteger()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const auto value = operand.AsInteger();
      if (value == std::numeric_limits<std::int64_t>::min()) {
        return std::unexpected(ExecutionStatus::IntegerOverflow);
      }
      values.subspan(operand_a).front() = runtime::Value::Integer(-value);
      return {};
    }
    case OpCode::NotBool:
      if (!values.subspan(operand_b).front().IsBoolean()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      values.subspan(operand_a).front() =
          runtime::Value::Boolean(!values.subspan(operand_b).front().AsBoolean());
      return {};
    case OpCode::EqualBool:
    case OpCode::NotEqualBool:
    case OpCode::AndBool:
    case OpCode::OrBool:
      return BooleanOperation(
          opcode, values.subspan(operand_a).front(),
          {.left = values.subspan(operand_b).front(), .right = values.subspan(operand_c).front()});
    default:
      return ExecuteObject(active, instruction);
  }
}

StepResult Execution::ExecuteObject(ActiveFrame& active, Instruction instruction) {
  auto values = active.frame.registers();
  const auto opcode = DecodeOp(instruction);
  const auto operand_a = DecodeA(instruction);
  const auto operand_b = DecodeB(instruction);
  const auto operand_c = DecodeC(instruction);
  auto& heap = isolate_.heap();
  switch (opcode) {
    case OpCode::NewOk:
    case OpCode::NewErr:
    case OpCode::ExpectedHasValue:
    case OpCode::ExpectedValue:
      return ExecuteExpected(active, instruction);
    case OpCode::LoadString:
      values.subspan(operand_a).front() =
          runtime::Value::String(std::span(active.function->strings->values)
                                     .subspan(DecodeUnsigned16(instruction))
                                     .front());
      return {};
    case OpCode::EqualValue:
    case OpCode::NotEqualValue: {
      const auto equal =
          IsEqualValue(values.subspan(operand_b).front(), values.subspan(operand_c).front());
      values.subspan(operand_a).front() =
          runtime::Value::Boolean(opcode == OpCode::EqualValue ? equal : !equal);
      return {};
    }
    case OpCode::BuiltinCall:
      return ExecuteBuiltin(active, instruction);
    case OpCode::StringInterpolate:
      return Interpolate(active, instruction);
    case OpCode::StringConcat:
    case OpCode::EqualString:
    case OpCode::NotEqualString:
      return StringOperation(opcode, values.subspan(operand_a).front(),
                             values.subspan(operand_b).front(), values.subspan(operand_c).front());
    case OpCode::NewArray:
    case OpCode::NewDynamicArray:
    case OpCode::NewInstance:
    case OpCode::NewCollection:
      return AllocateObject(active, instruction);
    case OpCode::GetIndex:
    case OpCode::SetIndex:
      return ArrayOperation(opcode, values.subspan(operand_a).front(),
                            values.subspan(operand_b).front(), values.subspan(operand_c).front());
    case OpCode::SetField: {
      const auto object = values.subspan(operand_a).front();
      if (!object.IsObject() || object.AsObject() == nullptr) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      if (!heap.SetField(*object.AsObject(), operand_b, values.subspan(operand_c).front())) {
        return std::unexpected(ExecutionStatus::InvalidField);
      }
      return {};
    }
    case OpCode::GetField: {
      const auto object = values.subspan(operand_b).front();
      if (!object.IsObject() || object.AsObject() == nullptr) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const auto* field = runtime::Heap::GetField(*object.AsObject(), operand_c);
      if (field == nullptr) {
        return std::unexpected(ExecutionStatus::InvalidField);
      }
      values.subspan(operand_a).front() = *field;
      return {};
    }
    case OpCode::IsType:
    case OpCode::CheckedCast: {
      const auto value = values.subspan(operand_b).front();
      const auto& type = active.function->layouts->types().subspan(operand_c).front();
      const bool matches = runtime::Heap::IsValueOfType(value, type);
      if (opcode == OpCode::CheckedCast && !matches) {
        return std::unexpected(ExecutionStatus::InvalidCast);
      }
      values.subspan(operand_a).front() =
          opcode == OpCode::IsType ? runtime::Value::Boolean(matches) : value;
      return {};
    }
    default:
      return ExecuteControl(active, instruction);
  }
}

StepResult Execution::AllocateObject(ActiveFrame& active, Instruction instruction) {
  const auto values = active.frame.registers();
  const auto opcode = DecodeOp(instruction);
  const auto operand_a = DecodeA(instruction);
  const auto operand_b = DecodeB(instruction);
  const auto operand_c = DecodeC(instruction);
  auto& heap = isolate_.heap();
  switch (opcode) {
    case OpCode::NewArray:
    case OpCode::NewDynamicArray:
    case OpCode::NewInstance: {
      std::size_t length = operand_c;
      if (opcode == OpCode::NewDynamicArray) {
        const auto size = values.subspan(operand_c).front();
        if (!size.IsInteger() || size.AsInteger() < 0) {
          return std::unexpected(ExecutionStatus::IndexOutOfBounds);
        }
        length = static_cast<std::size_t>(size.AsInteger());
      }
      auto* object =
          opcode == OpCode::NewArray || opcode == OpCode::NewDynamicArray
              ? heap.AllocateArray(*active.function->layouts->arrays().subspan(operand_b).front(),
                                   length)
              : heap.AllocateInstance(
                    *active.function->layouts->classes().subspan(operand_b).front());
      if (object == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      values.subspan(operand_a).front() = runtime::Value::Object(object);
      return {};
    }
    case OpCode::NewCollection: {
      const auto& layout = *active.function->layouts->collections().subspan(operand_b).front();
      auto* collection = heap.AllocateCollection(layout);
      if (collection == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      values.subspan(operand_a).front() = runtime::Value::Object(collection);
      return {};
    }
    default:
      std::unreachable();
  }
}

StepResult Execution::ExecuteControl(ActiveFrame& active, Instruction instruction) {
  const auto values = active.frame.registers();
  switch (DecodeOp(instruction)) {
    case OpCode::Jump:
      Jump(active, DecodeImm16(instruction));
      return {};
    case OpCode::JumpIfFalse: {
      const auto condition = values.subspan(DecodeA(instruction)).front();
      if (!condition.IsBoolean()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      if (!condition.AsBoolean()) {
        Jump(active, DecodeImm16(instruction));
      }
      return {};
    }
    case OpCode::Print:
      runtime::WriteValue(isolate_.output(), values.subspan(DecodeB(instruction)).front());
      isolate_.output() << '\n';
      return {};
    case OpCode::NativeCall:
      return ExecuteNative(active, instruction);
    case OpCode::Catch:
    case OpCode::ExceptionValue:
    case OpCode::Throw:
    case OpCode::Rethrow:
      return ExecuteException(active, instruction);
    case OpCode::Call:
      if (frame_count_ == kMaxCallFrames) {
        return std::unexpected(ExecutionStatus::StackOverflow);
      }
      return Call(active, instruction);
    case OpCode::CallIndirect:
      if (frame_count_ == kMaxCallFrames) {
        return std::unexpected(ExecutionStatus::StackOverflow);
      }
      return CallIndirect(active, instruction);
    case OpCode::CallInterface:
      if (frame_count_ == kMaxCallFrames) {
        return std::unexpected(ExecutionStatus::StackOverflow);
      }
      return CallInterface(active, instruction);
    case OpCode::GetGlobal:
    case OpCode::SetGlobal:
      return AccessGlobal(active, instruction);
    case OpCode::Return:
      Return(active, values.subspan(DecodeA(instruction)).front());
      return {};
    default:
      std::unreachable();
  }
}

StepResult Execution::ExecuteBuiltin(ActiveFrame& active, Instruction instruction) {
  const auto builtin = static_cast<Builtin>(DecodeB(instruction));
  auto values = active.frame.registers();
  const auto arguments = values.subspan(DecodeC(instruction), BuiltinArity(builtin));
  const auto result = [&]() -> std::expected<runtime::Value, runtime::BuiltinError> {
    if (builtin == Builtin::NumericToInt || builtin == Builtin::NumericToDouble) {
      return runtime::ConvertNumber(builtin == Builtin::NumericToInt
                                        ? runtime::NumericConversion::ToInt
                                        : runtime::NumericConversion::ToDouble,
                                    arguments.front().Read());
    }
    constexpr std::size_t kMaxBuiltinArguments = 3;
    std::array<runtime::Value, kMaxBuiltinArguments> materialized;
    const auto input = std::span(materialized).first(arguments.size());
    arguments.CopyTo(input);
    return runtime::ExecuteBuiltin(builtin, input, isolate_.heap());
  }();
  if (!result) {
    switch (result.error()) {
      case runtime::BuiltinError::OutOfMemory:
        return std::unexpected(ExecutionStatus::OutOfMemory);
      case runtime::BuiltinError::IndexOutOfBounds:
        return std::unexpected(ExecutionStatus::IndexOutOfBounds);
      case runtime::BuiltinError::TypeError:
        return std::unexpected(ExecutionStatus::TypeError);
      case runtime::BuiltinError::InvalidNumericConversion:
        return std::unexpected(ExecutionStatus::InvalidNumericConversion);
      case runtime::BuiltinError::ConcurrentModification:
        return std::unexpected(ExecutionStatus::ConcurrentModification);
    }
  }
  values.subspan(DecodeA(instruction)).front() = *result;
  return {};
}

StepResult Execution::ExecuteNative(ActiveFrame& active, Instruction instruction) {
  auto values = active.frame.registers();
  // 缓冲由 VM 持有，仅遇到更大的调用窗口时增长；只物化实际实参，不注册未用尾部。
  const auto count = DecodeC(instruction);
  if (native_arguments_->size() < count) {
    native_arguments_->resize(count);
  }
  const auto arguments = std::span(*native_arguments_).first(count);
  const auto source = values.subspan(DecodeB(instruction), arguments.size());
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    arguments.subspan(index).front() = source.subspan(index).front().Read();
  }
  const auto result = active.function->native->Invoke(isolate_, arguments);
  if (!result) {
    error_message_ = result.error().message;
    return std::unexpected(result.error().kind == runtime::NativeErrorKind::OutOfMemory
                               ? ExecutionStatus::OutOfMemory
                               : ExecutionStatus::NativeFailure);
  }
  values.subspan(DecodeA(instruction)).front() = *result;
  return {};
}

StepResult Execution::Interpolate(ActiveFrame& active, Instruction instruction) {
  auto values = active.frame.registers();
  const auto arguments = values.subspan(DecodeB(instruction), DecodeC(instruction));
  std::array<runtime::StringObject*, kMaxFrameRegisters> parts{};
  std::size_t index = 0;
  for (; index < arguments.size(); ++index) {
    const auto value = arguments.subspan(index).front();
    if (!value.IsString()) {
      break;
    }
    std::span(parts).subspan(index).front() = value.AsString();
  }
  auto& heap = isolate_.heap();
  const auto string = index == arguments.size()
                          ? std::expected<runtime::StringObject*, runtime::StringJoinError>(
                                heap.JoinStrings(std::span(parts).first(arguments.size())))
                          : heap.JoinValues(arguments);
  if (!string) {
    return std::unexpected(string.error() == runtime::StringJoinError::TypeMismatch
                               ? ExecutionStatus::TypeError
                               : ExecutionStatus::OutOfMemory);
  }
  if (*string == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  values.subspan(DecodeA(instruction)).front() = runtime::Value::String(*string);
  return {};
}

StepResult Execution::StringOperation(OpCode opcode,
                                      runtime::RegisterReference destination,
                                      runtime::Value left,
                                      runtime::Value right) {
  if (opcode == OpCode::StringConcat) {
    if (left.IsString() && right.IsString()) {
      auto* string =
          isolate_.heap().AllocateString(left.AsString()->value(), right.AsString()->value());
      if (string == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      destination = runtime::Value::String(string);
      return {};
    }
    const std::array parts = {left, right};
    const auto string = isolate_.heap().JoinValues(parts);
    if (!string) {
      return std::unexpected(string.error() == runtime::StringJoinError::TypeMismatch
                                 ? ExecutionStatus::TypeError
                                 : ExecutionStatus::OutOfMemory);
    }
    destination = runtime::Value::String(*string);
  } else {
    if (!left.IsString() || !right.IsString()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto left_text = left.AsString()->value();
    const auto right_text = right.AsString()->value();
    const bool equal = left_text == right_text;
    destination = runtime::Value::Boolean(opcode == OpCode::EqualString ? equal : !equal);
  }
  return {};
}

StepResult Execution::ArrayOperation(OpCode opcode,
                                     runtime::RegisterReference destination,
                                     runtime::Value left,
                                     runtime::Value right) {
  const auto object = opcode == OpCode::GetIndex ? left : destination;
  const auto index = opcode == OpCode::GetIndex ? right : left;
  if (!object.IsObject() || object.AsObject() == nullptr || !index.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto integer_index = index.AsInteger();
  if (integer_index < 0 ||
      std::cmp_greater(integer_index, std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  const auto offset = static_cast<std::uint32_t>(integer_index);
  auto& heap = isolate_.heap();
  if (opcode == OpCode::SetIndex) {
    const auto stored = heap.SetElement(*object.AsObject(), offset, right);
    if (!stored) {
      return std::unexpected(stored.error() == runtime::ArrayAccessError::OutOfBounds
                                 ? ExecutionStatus::IndexOutOfBounds
                                 : ExecutionStatus::TypeError);
    }
  } else {
    const auto element = runtime::Heap::GetElement(*object.AsObject(), offset);
    if (!element) {
      return std::unexpected(element.error() == runtime::ArrayAccessError::OutOfBounds
                                 ? ExecutionStatus::IndexOutOfBounds
                                 : ExecutionStatus::TypeError);
    }
    destination = *element;
  }
  return {};
}

void Execution::Jump(ActiveFrame& active, std::int16_t offset) noexcept {
  const auto target = static_cast<std::int64_t>(active.program_counter) + offset;
  active.program_counter = static_cast<std::size_t>(target);
}

StepResult Execution::ExecuteExpected(ActiveFrame& active, Instruction instruction) {
  const auto values = active.frame.registers();
  const auto opcode = DecodeOp(instruction);
  auto destination = values.subspan(DecodeA(instruction)).front();
  if (opcode == OpCode::NewOk || opcode == OpCode::NewErr) {
    const auto payload = values.subspan(DecodeC(instruction)).front();
    auto& layout = *active.function->layouts->classes().subspan(DecodeB(instruction)).front();
    auto* object = isolate_.heap().AllocateExpected(layout, payload,
                                                    opcode == OpCode::NewOk
                                                        ? runtime::ExpectedAlternative::Ok
                                                        : runtime::ExpectedAlternative::Err);
    if (object == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    destination = runtime::Value::Object(object);
    return {};
  }
  const auto receiver = values.subspan(DecodeB(instruction)).front();
  if (!receiver.IsObject() || receiver.AsObject() == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (opcode == OpCode::ExpectedHasValue) {
    const auto alternative = runtime::Heap::ExpectedAlternativeOf(*receiver.AsObject());
    if (!alternative) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    destination = runtime::Value::Boolean(*alternative == runtime::ExpectedAlternative::Ok);
    return {};
  }
  const auto payload = runtime::Heap::ReadExpectedValue(
      *receiver.AsObject(), static_cast<runtime::ExpectedAlternative>(DecodeC(instruction)));
  if (!payload) {
    return std::unexpected(payload.error() == runtime::ExpectedAccessError::InactiveAlternative
                               ? ExecutionStatus::InactiveExpectedAlternative
                               : ExecutionStatus::TypeError);
  }
  destination = *payload;
  return {};
}

StepResult Execution::Call(ActiveFrame& active, Instruction instruction) {
  return EnterCall(DecodeB(instruction), active, instruction, std::nullopt);
}

StepResult Execution::AccessGlobal(ActiveFrame& active, Instruction instruction) {
  const auto index = DecodeB(instruction);
  auto& state = globals_->states.at(index);
  auto& value = globals_->values.at(index);
  auto slot = active.frame.registers().subspan(DecodeA(instruction)).front();
  if (DecodeOp(instruction) == OpCode::SetGlobal) {
    value = slot;
    state = GlobalState::Ready;
    return {};
  }
  switch (state) {
    case GlobalState::Ready:
      slot = value;
      return {};
    case GlobalState::Initializing:
      return std::unexpected(ExecutionStatus::CyclicInitialization);
    case GlobalState::Uninitialized:
      break;
  }
  if (frame_count_ == kMaxCallFrames) {
    return std::unexpected(ExecutionStatus::StackOverflow);
  }
  state = GlobalState::Initializing;
  const auto entered =
      EnterCall(globals_->initializers.at(index), active, instruction, std::nullopt);
  if (entered) {
    std::span(frames_).subspan(frame_count_ - 1).front().initialized_global = index;
  }
  return entered;
}

std::expected<std::uint32_t, ExecutionStatus> Execution::InterfaceTarget(
    runtime::Value receiver,
    std::uint32_t selector) const {
  if (!receiver.IsObject() || receiver.AsObject() == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* layout = runtime::Heap::ClassOf(*receiver.AsObject());
  if (layout == nullptr || selector >= layout->dispatch_targets().size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto target = layout->dispatch_targets().subspan(selector).front();
  if (target >= functions_.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return target;
}

StepResult Execution::CallInterface(ActiveFrame& active, Instruction instruction) {
  const auto base = DecodeC(instruction);
  const auto values = active.frame.registers();
  const auto target = InterfaceTarget(values.subspan(base).front(), DecodeB(instruction));
  if (!target) {
    return std::unexpected(target.error());
  }
  if (static_cast<std::size_t>(base) + functions_.subspan(*target).front().parameter_count >
      values.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return EnterCall(*target, active, instruction, std::nullopt);
}

StepResult Execution::CallIndirect(ActiveFrame& active, Instruction instruction) {
  const auto value = active.frame.registers().subspan(DecodeB(instruction)).front();
  if (!value.IsObject() || value.AsObject() == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* layout = runtime::Heap::ClassOf(*value.AsObject());
  if (layout == nullptr || !layout->callable()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto target = *layout->callable();
  if (target.mode != CallableMode::InterfaceMethod && target.function >= functions_.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  std::optional<runtime::Value> implicit;
  if (target.mode == CallableMode::Closure) {
    implicit = value.Read();
  } else if (target.mode == CallableMode::BoundMethod ||
             target.mode == CallableMode::InterfaceMethod) {
    const auto* receiver = runtime::Heap::GetField(*value.AsObject(), 0);
    if (receiver == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    implicit = *receiver;
    if (target.mode == CallableMode::InterfaceMethod) {
      const auto method = InterfaceTarget(*receiver, target.function);
      if (!method) {
        return std::unexpected(method.error());
      }
      target.function = *method;
    }
  }
  const auto count = functions_.subspan(target.function).front().parameter_count;
  const auto hidden = implicit.has_value() ? 1U : 0U;
  if (count < hidden || static_cast<std::size_t>(DecodeC(instruction)) + count - hidden >
                            active.frame.registers().size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return EnterCall(target.function, active, instruction, implicit);
}

StepResult Execution::EnterCall(std::uint32_t target,
                                ActiveFrame& active,
                                Instruction instruction,
                                std::optional<runtime::Value> implicit) {
  const auto& callee = functions_.subspan(target).front();
  auto callee_values = registers_.subspan(frame_count_).front().Acquire(callee.register_count);
  if (callee_values.IsEmpty()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  auto& next = std::span(frames_).subspan(frame_count_).front();
  next.function = &callee;
  next.program_counter = 0;
  next.return_register = DecodeA(instruction);
  next.initialized_global.reset();
  next.frame.Attach(isolate_.heap(), callee_values);
  const std::size_t hidden = implicit.has_value() ? 1 : 0;
  if (implicit) {
    callee_values.front() = *implicit;
  }
  callee_values.subspan(hidden, callee.parameter_count - hidden)
      .CopyFrom(
          active.frame.registers().subspan(DecodeC(instruction), callee.parameter_count - hidden));
  ++frame_count_;
  return {};
}

void Execution::Return(ActiveFrame& active, runtime::Value value) {
  if (active.initialized_global) {
    const auto index = *active.initialized_global;
    globals_->values.at(index) = value;
    globals_->states.at(index) = GlobalState::Ready;
  }
  const auto return_register = active.return_register;
  active.frame.Detach();
  --frame_count_;
  if (frame_count_ == 0) {
    result_ = value;
    return;
  }
  auto& caller = std::span(frames_).subspan(frame_count_ - 1).front();
  caller.frame.registers().subspan(return_register).front() = value;
}

}  // namespace

ExecutionResult VirtualMachine::Execute(const FunctionHandle& function) noexcept {
  return Execute(function.program(), function.index());
}

std::size_t VirtualMachine::register_bytes() const noexcept {
  std::size_t bytes = 0;
  for (const auto& registers : registers_) {
    bytes += registers.capacity() * runtime::kRegisterSlotBytes;
  }
  return bytes;
}

ExecutionResult VirtualMachine::Execute(const Program& program,
                                        std::uint32_t entry_function) noexcept {
  return ExecuteImpl(program, entry_function, nullptr);
}

ExecutionResult VirtualMachine::Debug(const FunctionHandle& function, Debugger& debugger) noexcept {
  return ExecuteImpl(function.program(), function.index(), &debugger);
}

ExecutionResult VirtualMachine::ExecuteImpl(const Program& program,
                                            std::uint32_t entry_function,
                                            Debugger* debugger) noexcept {
  const auto functions = program.functions();
  if (entry_function >= functions.size() ||
      functions.subspan(entry_function).front().parameter_count != 0) {
    return {
        .value = runtime::Value::Null(),
        .status = ExecutionStatus::InvalidEntry,
        .stack_trace = {},
        .message = {},
    };
  }
  if (!program.IsForIsolate(isolate_)) {
    return {
        .value = runtime::Value::Null(),
        .status = ExecutionStatus::InvalidIsolate,
        .stack_trace = {},
        .message = {},
    };
  }
  internal::Execution execution(isolate_, program, registers_, native_arguments_);
  return debugger != nullptr ? execution.DebugRun(entry_function, *debugger)
                             : execution.Run(entry_function);
}

}  // namespace vela::vm
