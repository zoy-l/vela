#include "vm/vm.h"

#include <algorithm>
#include <array>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include "runtime/string.h"
#include "vm/frame.h"

namespace vela::vm {
namespace {

constexpr std::size_t kMaxRegisters = 256;
constexpr std::size_t kMaxFrames = 64;
using StepResult = std::expected<void, ExecutionStatus>;

auto IsRegister(std::uint8_t index, const Function& function) noexcept -> bool {
  return index < function.register_count;
}

auto IsValidInstruction(Instruction instruction,
                        const Function& function,
                        std::span<const Function> functions) noexcept -> bool {
  const OpCode opcode = DecodeOp(instruction);
  const auto operand_a = DecodeA(instruction);
  const auto operand_b = DecodeB(instruction);
  const auto operand_c = DecodeC(instruction);
  switch (opcode) {
    case OpCode::LoadNull:
    case OpCode::LoadBool:
    case OpCode::LoadInt:
    case OpCode::Return:
    case OpCode::NewArray:
      return IsRegister(operand_a, function);
    case OpCode::Move:
    case OpCode::NegInt:
    case OpCode::NotBool:
    case OpCode::GetField:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function);
    case OpCode::AddInt:
    case OpCode::SubInt:
    case OpCode::MulInt:
    case OpCode::DivInt:
    case OpCode::EqualInt:
    case OpCode::NotEqualInt:
    case OpCode::EqualBool:
    case OpCode::NotEqualBool:
    case OpCode::LessInt:
    case OpCode::LessEqualInt:
    case OpCode::GreaterInt:
    case OpCode::GreaterEqualInt:
    case OpCode::AndBool:
    case OpCode::OrBool:
    case OpCode::StringConcat:
    case OpCode::EqualString:
    case OpCode::NotEqualString:
    case OpCode::GetIndex:
    case OpCode::SetIndex:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function) &&
             IsRegister(operand_c, function);
    case OpCode::Call:
      return IsRegister(operand_a, function) && operand_b < functions.size() &&
             static_cast<std::size_t>(operand_c) +
                     functions.subspan(operand_b).front().parameter_count <=
                 function.register_count;
    case OpCode::Jump:
      return true;
    case OpCode::JumpIfFalse:
      return IsRegister(operand_a, function);
    case OpCode::NewInstance:
      return IsRegister(operand_a, function) && operand_b < function.classes.size() &&
             function.classes.subspan(operand_b).front() != nullptr;
    case OpCode::LoadString:
      return IsRegister(operand_a, function) && operand_b < function.strings.size() &&
             function.strings.subspan(operand_b).front() != nullptr;
    case OpCode::Print:
      return IsRegister(operand_b, function);
    case OpCode::SetField:
      return IsRegister(operand_a, function) && IsRegister(operand_c, function);
  }
  return false;
}

auto IntegerArithmetic(OpCode opcode,
                       runtime::Value& destination,
                       std::int64_t left,
                       std::int64_t right) noexcept -> StepResult {
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
      return std::unexpected(ExecutionStatus::InvalidBytecode);
  }
  if (overflow) {
    return std::unexpected(ExecutionStatus::IntegerOverflow);
  }
  destination = runtime::Value::Integer(result);
  return {};
}

auto IntegerOperation(OpCode opcode,
                      runtime::Value& destination,
                      runtime::Value left_value,
                      runtime::Value right_value) noexcept -> StepResult {
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

auto BooleanOperation(OpCode opcode,
                      runtime::Value& destination,
                      runtime::Value left_value,
                      runtime::Value right_value) noexcept -> StepResult {
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
      return std::unexpected(ExecutionStatus::InvalidBytecode);
  }
  destination = runtime::Value::Boolean(result);
  return {};
}

auto Negate(runtime::Value& destination, runtime::Value operand) noexcept -> StepResult {
  if (!operand.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto value = operand.AsInteger();
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return std::unexpected(ExecutionStatus::IntegerOverflow);
  }
  destination = runtime::Value::Integer(-value);
  return {};
}

void PrintValue(runtime::Value value) {
  if (value.IsInteger()) {
    std::cout << value.AsInteger();
  } else if (value.IsBoolean()) {
    std::cout << (value.AsBoolean() ? "true" : "false");
  } else if (value.IsString()) {
    std::cout << value.AsString()->value;
  } else if (value.IsNull()) {
    std::cout << "null";
  } else {
    std::cout << "object";
  }
  std::cout << '\n';
}

class Execution final {
 public:
  Execution(runtime::Isolate& isolate, const Program& program)
      : isolate_(isolate), program_(program) {}

  auto Run(std::uint32_t entry_function) noexcept -> ExecutionResult;

 private:
  struct ActiveFrame final {
    Frame frame;
    const Function* function = nullptr;
    std::size_t program_counter = 0;
    std::uint8_t return_register = 0;
  };
  struct Registers final {
    std::array<runtime::Value, kMaxRegisters * kMaxFrames> values;
  };

  auto ExecuteInstruction(ActiveFrame& active, Instruction instruction) -> StepResult;
  auto ExecuteControl(ActiveFrame& active, Instruction instruction) -> StepResult;
  auto ExecuteObject(ActiveFrame& active, Instruction instruction) -> StepResult;
  auto StringOperation(OpCode opcode,
                       runtime::Value& destination,
                       runtime::Value left,
                       runtime::Value right) -> StepResult;
  auto ArrayOperation(OpCode opcode,
                      runtime::Value& destination,
                      runtime::Value left,
                      runtime::Value right) -> StepResult;
  static auto Jump(ActiveFrame& active, std::int16_t offset) noexcept -> StepResult;
  void Call(ActiveFrame& active, Instruction instruction);
  void Return(ActiveFrame& active, runtime::Value value);

  runtime::Isolate& isolate_;
  const Program& program_;
  std::unique_ptr<Registers> registers_;
  std::array<ActiveFrame, kMaxFrames> frames_;
  std::size_t frame_count_ = 0;
  runtime::Value result_;
};

auto Execution::Run(std::uint32_t entry_function) noexcept -> ExecutionResult {
  registers_ = std::unique_ptr<Registers>(new (std::nothrow) Registers());
  if (registers_ == nullptr) {
    return {.value = runtime::Value::Null(), .status = ExecutionStatus::OutOfMemory};
  }
  auto& entry = frames_.front();
  entry.function = &program_.functions.subspan(entry_function).front();
  entry.frame.Attach(isolate_.heap(),
                     std::span(registers_->values).first(entry.function->register_count));
  frame_count_ = 1;
  while (frame_count_ != 0) {
    auto& active = std::span(frames_).subspan(frame_count_ - 1).front();
    const auto code = active.function->code;
    if (active.program_counter >= code.size()) {
      return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
    }
    const auto instruction = code.subspan(active.program_counter++).front();
    const auto step = ExecuteInstruction(active, instruction);
    if (!step) {
      return {.value = runtime::Value::Null(), .status = step.error()};
    }
  }
  return {.value = result_, .status = ExecutionStatus::Returned};
}

auto Execution::ExecuteInstruction(ActiveFrame& active, Instruction instruction) -> StepResult {
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
      return IntegerOperation(opcode, values.subspan(operand_a).front(),
                              values.subspan(operand_b).front(), values.subspan(operand_c).front());
    case OpCode::NegInt:
      return Negate(values.subspan(operand_a).front(), values.subspan(operand_b).front());
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
      return BooleanOperation(opcode, values.subspan(operand_a).front(),
                              values.subspan(operand_b).front(), values.subspan(operand_c).front());
    default:
      return ExecuteObject(active, instruction);
  }
}

auto Execution::ExecuteObject(ActiveFrame& active, Instruction instruction) -> StepResult {
  auto values = active.frame.registers();
  const auto opcode = DecodeOp(instruction);
  const auto operand_a = DecodeA(instruction);
  const auto operand_b = DecodeB(instruction);
  const auto operand_c = DecodeC(instruction);
  auto& heap = isolate_.heap();
  switch (opcode) {
    case OpCode::LoadString:
      values.subspan(operand_a).front() =
          runtime::Value::String(active.function->strings.subspan(operand_b).front());
      return {};
    case OpCode::StringConcat:
    case OpCode::EqualString:
    case OpCode::NotEqualString:
      return StringOperation(opcode, values.subspan(operand_a).front(),
                             values.subspan(operand_b).front(), values.subspan(operand_c).front());
    case OpCode::NewArray:
    case OpCode::NewInstance: {
      auto* object =
          opcode == OpCode::NewArray
              ? heap.AllocateArray(operand_b)
              : heap.AllocateInstance(*active.function->classes.subspan(operand_b).front());
      if (object == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      values.subspan(operand_a).front() = runtime::Value::Object(object);
      return {};
    }
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
    default:
      return ExecuteControl(active, instruction);
  }
}

auto Execution::ExecuteControl(ActiveFrame& active, Instruction instruction) -> StepResult {
  const auto values = active.frame.registers();
  switch (DecodeOp(instruction)) {
    case OpCode::Jump:
      return Jump(active, DecodeImm16(instruction));
    case OpCode::JumpIfFalse: {
      const auto condition = values.subspan(DecodeA(instruction)).front();
      if (!condition.IsBoolean()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      if (!condition.AsBoolean()) {
        return Jump(active, DecodeImm16(instruction));
      }
      return {};
    }
    case OpCode::Print:
      PrintValue(values.subspan(DecodeB(instruction)).front());
      return {};
    case OpCode::Call:
      if (frame_count_ == kMaxFrames) {
        return std::unexpected(ExecutionStatus::StackOverflow);
      }
      Call(active, instruction);
      return {};
    case OpCode::Return:
      Return(active, values.subspan(DecodeA(instruction)).front());
      return {};
    default:
      return std::unexpected(ExecutionStatus::InvalidBytecode);
  }
}

auto Execution::StringOperation(OpCode opcode,
                                runtime::Value& destination,
                                runtime::Value left,
                                runtime::Value right) -> StepResult {
  if (!left.IsString() || !right.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& left_text = left.AsString()->value;
  const auto& right_text = right.AsString()->value;
  if (opcode == OpCode::StringConcat) {
    destination = runtime::Value::String(isolate_.CreateString(left_text + right_text));
  } else {
    const bool equal = left_text == right_text;
    destination = runtime::Value::Boolean(opcode == OpCode::EqualString ? equal : !equal);
  }
  return {};
}

auto Execution::ArrayOperation(OpCode opcode,
                               runtime::Value& destination,
                               runtime::Value left,
                               runtime::Value right) -> StepResult {
  const auto object = opcode == OpCode::GetIndex ? left : destination;
  const auto index = opcode == OpCode::GetIndex ? right : left;
  if (!object.IsObject() || object.AsObject() == nullptr || !index.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto integer_index = index.AsInteger();
  if (integer_index < 0 ||
      std::cmp_greater(integer_index, std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected(ExecutionStatus::InvalidField);
  }
  const auto offset = static_cast<std::uint32_t>(integer_index);
  auto& heap = isolate_.heap();
  if (opcode == OpCode::SetIndex) {
    if (!heap.SetElement(*object.AsObject(), offset, right)) {
      return std::unexpected(ExecutionStatus::InvalidField);
    }
  } else {
    const auto* element = runtime::Heap::GetElement(*object.AsObject(), offset);
    if (element == nullptr) {
      return std::unexpected(ExecutionStatus::InvalidField);
    }
    destination = *element;
  }
  return {};
}

auto Execution::Jump(ActiveFrame& active, std::int16_t offset) noexcept -> StepResult {
  const auto target = static_cast<std::int64_t>(active.program_counter) + offset;
  if (target < 0 || std::cmp_greater(target, active.function->code.size())) {
    return std::unexpected(ExecutionStatus::InvalidBytecode);
  }
  active.program_counter = static_cast<std::size_t>(target);
  return {};
}

void Execution::Call(ActiveFrame& active, Instruction instruction) {
  const auto& callee = program_.functions.subspan(DecodeB(instruction)).front();
  auto& next = std::span(frames_).subspan(frame_count_).front();
  auto callee_values =
      std::span(registers_->values).subspan(frame_count_ * kMaxRegisters, callee.register_count);
  next.function = &callee;
  next.program_counter = 0;
  next.return_register = DecodeA(instruction);
  next.frame.Attach(isolate_.heap(), callee_values);
  std::ranges::copy(active.frame.registers().subspan(DecodeC(instruction), callee.parameter_count),
                    callee_values.begin());
  ++frame_count_;
}

void Execution::Return(ActiveFrame& active, runtime::Value value) {
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

auto VirtualMachine::Validate(const Function& function,
                              std::span<const Function> functions) noexcept -> bool {
  if (function.code.empty() || function.register_count == 0 ||
      function.parameter_count > function.register_count) {
    return false;
  }
  return std::ranges::all_of(function.code, [&](Instruction instruction) -> bool {
    return IsValidInstruction(instruction, function, functions);
  });
}

auto VirtualMachine::Execute(const Function& function) noexcept -> ExecutionResult {
  const std::array<Function, 1> functions = {function};
  return Execute(Program{.functions = functions}, 0);
}

auto VirtualMachine::Execute(const Program& program, std::uint32_t entry_function) noexcept
    -> ExecutionResult {
  if (entry_function >= program.functions.size()) {
    return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
  }
  for (const auto& function : program.functions) {
    if (!Validate(function, program.functions)) {
      return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
    }
  }
  return Execution(isolate_, program).Run(entry_function);
}

}  // namespace vela::vm
