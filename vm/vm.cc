#include "vm/vm.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "vm/frame.h"

namespace vela::vm {
namespace {

constexpr std::size_t kMaxRegisters = 256;

[[nodiscard]] bool IsRegister(std::uint8_t index, std::uint8_t register_count) noexcept {
  return index < register_count;
}

[[nodiscard]] bool IsIntegerAdditionSafe(std::int64_t left, std::int64_t right) noexcept {
  if (right > 0) {
    return left <= std::numeric_limits<std::int64_t>::max() - right;
  }
  if (right < 0) {
    return left >= std::numeric_limits<std::int64_t>::min() - right;
  }
  return true;
}

}  // namespace

bool VirtualMachine::Validate(const Function& function) noexcept {
  if (function.code.empty() || function.register_count == 0) {
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
        if (!IsRegister(a, function.register_count) || !IsRegister(b, function.register_count) ||
            !IsRegister(c, function.register_count)) {
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
  if (!Validate(function)) {
    return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
  }

  std::array<runtime::Value, kMaxRegisters> register_storage{};
  Frame frame(isolate_.heap(),
              std::span<runtime::Value>(register_storage.data(), function.register_count));
  runtime::Value* const registers = frame.data();
  runtime::Heap& heap = isolate_.heap();

  std::size_t program_counter = 0;
  while (program_counter < function.code.size()) {
    const Instruction instruction = function.code[program_counter++];
    const OpCode opcode = DecodeOp(instruction);
    const std::uint8_t a = DecodeA(instruction);
    const std::uint8_t b = DecodeB(instruction);
    const std::uint8_t c = DecodeC(instruction);

    switch (opcode) {
      case OpCode::LoadNull:
        registers[a] = runtime::Value::Null();
        break;
      case OpCode::LoadBool:
        registers[a] = runtime::Value::Boolean(b != 0);
        break;
      case OpCode::LoadInt:
        registers[a] = runtime::Value::Integer(DecodeImm16(instruction));
        break;
      case OpCode::Move:
        registers[a] = registers[b];
        break;
      case OpCode::AddInt: {
        if (!registers[b].IsInteger() || !registers[c].IsInteger()) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        const std::int64_t left = registers[b].AsInteger();
        const std::int64_t right = registers[c].AsInteger();
        if (!IsIntegerAdditionSafe(left, right)) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::IntegerOverflow};
        }
        registers[a] = runtime::Value::Integer(left + right);
        break;
      }
      case OpCode::NewInstance: {
        runtime::HeapObject* object = heap.AllocateInstance(*function.classes[b]);
        if (object == nullptr) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::OutOfMemory};
        }
        registers[a] = runtime::Value::Object(object);
        break;
      }
      case OpCode::SetField: {
        if (!registers[a].IsObject() || registers[a].AsObject() == nullptr) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        if (!heap.SetField(*registers[a].AsObject(), b, registers[c])) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidField};
        }
        break;
      }
      case OpCode::GetField: {
        if (!registers[b].IsObject() || registers[b].AsObject() == nullptr) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::TypeError};
        }
        const runtime::Value* field = heap.GetField(*registers[b].AsObject(), c);
        if (field == nullptr) {
          return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidField};
        }
        registers[a] = *field;
        break;
      }
      case OpCode::Return:
        return {.value = registers[a], .status = ExecutionStatus::Returned};
      default:
        return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
    }
  }

  return {.value = runtime::Value::Null(), .status = ExecutionStatus::InvalidBytecode};
}

}  // namespace vela::vm
