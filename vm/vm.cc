#include "vm/vm.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>

#include "vm/frame.h"

namespace vela::vm {
namespace {

constexpr std::size_t kMaxRegisters = 256;
constexpr std::size_t kMaxFrames = 64;

[[nodiscard]] bool IsRegister(std::uint8_t index, std::uint8_t register_count) noexcept {
  return index < register_count;
}

[[nodiscard]] bool AddInt(std::int64_t left, std::int64_t right, std::int64_t* result) noexcept {
  return __builtin_add_overflow(left, right, result) == 0;
}

[[nodiscard]] bool SubInt(std::int64_t left, std::int64_t right, std::int64_t* result) noexcept {
  return __builtin_sub_overflow(left, right, result) == 0;
}

[[nodiscard]] bool MulInt(std::int64_t left, std::int64_t right, std::int64_t* result) noexcept {
  return __builtin_mul_overflow(left, right, result) == 0;
}

[[nodiscard]] bool ApplyJump(std::size_t* program_counter,
                             std::int16_t offset,
                             std::size_t code_size) noexcept {
  const std::int64_t target = static_cast<std::int64_t>(*program_counter) + offset;
  if (target < 0 || static_cast<std::uint64_t>(target) > code_size) {
    return false;
  }
  *program_counter = static_cast<std::size_t>(target);
  return true;
}

}  // namespace

bool VirtualMachine::Validate(const Function& function,
                              std::span<const Function> functions) noexcept {
  if (function.code.empty() || function.register_count == 0 ||
      function.parameter_count > function.register_count) {
    return false;
  }

  for (Instruction instruction : function.code) {
    const OpCode opcode = DecodeOp(instruction);
    const std::uint8_t a = DecodeA(instruction);
    const std::uint8_t b = DecodeB(instruction);
    const std::uint8_t c = DecodeC(instruction);

    switch (opcode) {
      case OpCode::LoadNull:
      case OpCode::LoadBool:
      case OpCode::LoadInt:
      case OpCode::Return:
        if (!IsRegister(a, function.register_count)) {
          return false;
        }
        break;
      case OpCode::Move:
        if (!IsRegister(a, function.register_count) || !IsRegister(b, function.register_count)) {
          return false;
        }
        break;
      case OpCode::AddInt:
      case OpCode::SubInt:
      case OpCode::MulInt:
      case OpCode::EqualInt:
      case OpCode::NotEqualInt:
      case OpCode::EqualBool:
      case OpCode::NotEqualBool:
        if (!IsRegister(a, function.register_count) || !IsRegister(b, function.register_count) ||
            !IsRegister(c, function.register_count)) {
          return false;
        }
        break;
      case OpCode::DivInt:
      case OpCode::NegInt:
      case OpCode::NotBool:
        if (!IsRegister(a, function.register_count) || !IsRegister(b, function.register_count)) {
          return false;
        }
        break;
      case OpCode::Call:
        if (!IsRegister(a, function.register_count) || b >= functions.size() ||
            static_cast<std::size_t>(c) + functions[b].parameter_count > function.register_count) {
          return false;
        }
        break;
      case OpCode::Jump:
        break;
      case OpCode::JumpIfFalse:
        if (!IsRegister(a, function.register_count)) {
          return false;
        }
        break;
      case OpCode::NewInstance:
        if (!IsRegister(a, function.register_count) || b >= function.classes.size()) {
          return false;
        }
        break;
      case OpCode::SetField:
        if (!IsRegister(a, function.register_count) || !IsRegister(c, function.register_count)) {
          return false;
        }
        break;
      case OpCode::GetField:
        if (!IsRegister(a, function.register_count) || !IsRegister(b, function.register_count)) {
          return false;
        }
        break;
      default:
        return false;
    }
  }
  return true;
}

ExecutionResult VirtualMachine::Execute(const Function& function) noexcept {
  const std::array<Function, 1> functions = {function};
  return Execute(Program{functions}, 0);
}

ExecutionResult VirtualMachine::Execute(const Program& program,
                                        std::uint32_t entry_function) noexcept {
  if (entry_function >= program.functions.size()) {
    return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
  }
  for (const Function& function : program.functions) {
    if (!Validate(function, program.functions)) {
      return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
    }
  }

  auto registers = std::unique_ptr<runtime::Value[]>(
      new (std::nothrow) runtime::Value[kMaxRegisters * kMaxFrames]);
  if (registers == nullptr) {
    return {.value = runtime::Value::Null(), .status = ExecutionStatus::OutOfMemory};
  }

  struct ActiveFrame final {
    Frame frame;
    const Function* function = nullptr;
    std::size_t program_counter = 0;
    std::uint8_t return_register = 0;
  };

  std::array<ActiveFrame, kMaxFrames> frames;
  runtime::Heap& heap = isolate_.heap();
  std::size_t frame_count = 1;
  std::size_t current_frame = 0;
  frames[0].function = &program.functions[entry_function];
  frames[0].frame.Attach(
      heap, std::span<runtime::Value>(registers.get(), frames[0].function->register_count));

  while (frame_count != 0) {
    ActiveFrame& active = frames[current_frame];
    const Function& function = *active.function;
    runtime::Value* const values = active.frame.data();
    if (active.program_counter >= function.code.size()) {
      return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
    }

    const Instruction instruction = function.code[active.program_counter++];
    const OpCode opcode = DecodeOp(instruction);
    const std::uint8_t a = DecodeA(instruction);
    const std::uint8_t b = DecodeB(instruction);
    const std::uint8_t c = DecodeC(instruction);

    switch (opcode) {
      case OpCode::LoadNull:
        values[a] = runtime::Value::Null();
        break;
      case OpCode::LoadBool:
        values[a] = runtime::Value::Boolean(b != 0);
        break;
      case OpCode::LoadInt:
        values[a] = runtime::Value::Integer(DecodeImm16(instruction));
        break;
      case OpCode::Move:
        values[a] = values[b];
        break;
      case OpCode::AddInt:
      case OpCode::SubInt:
      case OpCode::MulInt: {
        if (!values[b].IsInteger() || !values[c].IsInteger()) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        const std::int64_t left = values[b].AsInteger();
        const std::int64_t right = values[c].AsInteger();
        std::int64_t result = 0;
        const bool valid = opcode == OpCode::AddInt   ? AddInt(left, right, &result)
                           : opcode == OpCode::SubInt ? SubInt(left, right, &result)
                                                      : MulInt(left, right, &result);
        if (!valid) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::IntegerOverflow};
        }
        values[a] = runtime::Value::Integer(result);
        break;
      }
      case OpCode::DivInt: {
        if (!values[b].IsInteger() || !values[c].IsInteger()) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        const std::int64_t left = values[b].AsInteger();
        const std::int64_t right = values[c].AsInteger();
        if (right == 0) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::DivisionByZero};
        }
        if (left == std::numeric_limits<std::int64_t>::min() && right == -1) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::IntegerOverflow};
        }
        values[a] = runtime::Value::Integer(left / right);
        break;
      }
      case OpCode::NegInt: {
        if (!values[b].IsInteger()) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        const std::int64_t value = values[b].AsInteger();
        if (value == std::numeric_limits<std::int64_t>::min()) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::IntegerOverflow};
        }
        values[a] = runtime::Value::Integer(-value);
        break;
      }
      case OpCode::NotBool:
        if (!values[b].IsBoolean()) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        values[a] = runtime::Value::Boolean(!values[b].AsBoolean());
        break;
      case OpCode::EqualInt:
      case OpCode::NotEqualInt:
        if (!values[b].IsInteger() || !values[c].IsInteger()) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        values[a] = runtime::Value::Boolean(opcode == OpCode::EqualInt
                                                ? values[b].AsInteger() == values[c].AsInteger()
                                                : values[b].AsInteger() != values[c].AsInteger());
        break;
      case OpCode::EqualBool:
      case OpCode::NotEqualBool:
        if (!values[b].IsBoolean() || !values[c].IsBoolean()) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        values[a] = runtime::Value::Boolean(opcode == OpCode::EqualBool
                                                ? values[b].AsBoolean() == values[c].AsBoolean()
                                                : values[b].AsBoolean() != values[c].AsBoolean());
        break;
      case OpCode::Call: {
        if (frame_count == kMaxFrames) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::StackOverflow};
        }
        const Function& callee = program.functions[b];
        ActiveFrame& next = frames[frame_count];
        runtime::Value* const callee_values = registers.get() + frame_count * kMaxRegisters;
        next.function = &callee;
        next.program_counter = 0;
        next.return_register = a;
        next.frame.Attach(heap, std::span<runtime::Value>(callee_values, callee.register_count));
        for (std::size_t index = 0; index < callee.parameter_count; ++index) {
          callee_values[index] = values[static_cast<std::size_t>(c) + index];
        }
        current_frame = frame_count;
        ++frame_count;
        break;
      }
      case OpCode::Jump:
        if (!ApplyJump(&active.program_counter, DecodeImm16(instruction), function.code.size())) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
        }
        break;
      case OpCode::JumpIfFalse:
        if (!values[a].IsBoolean()) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        if (!values[a].AsBoolean() &&
            !ApplyJump(&active.program_counter, DecodeImm16(instruction), function.code.size())) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
        }
        break;
      case OpCode::NewInstance: {
        runtime::HeapObject* object = heap.AllocateInstance(*function.classes[b]);
        if (object == nullptr) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::OutOfMemory};
        }
        values[a] = runtime::Value::Object(object);
        break;
      }
      case OpCode::SetField: {
        if (!values[a].IsObject() || values[a].AsObject() == nullptr) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        if (!heap.SetField(*values[a].AsObject(), b, values[c])) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidField};
        }
        break;
      }
      case OpCode::GetField: {
        if (!values[b].IsObject() || values[b].AsObject() == nullptr) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        const runtime::Value* field = heap.GetField(*values[b].AsObject(), c);
        if (field == nullptr) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidField};
        }
        values[a] = *field;
        break;
      }
      case OpCode::Return: {
        const runtime::Value result = values[a];
        if (current_frame == 0) {
          return {.value = result, .status = ExecutionStatus::Returned};
        }
        const std::uint8_t return_register = active.return_register;
        active.frame.Detach();
        --frame_count;
        --current_frame;
        frames[current_frame].frame.data()[return_register] = result;
        break;
      }
      default:
        return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
    }
  }

  return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
}

}  // namespace vela::vm
