#include <array>
#include <cstdint>
#include <iostream>

#include "vm/vm.h"

namespace {

using vela::runtime::GcPhase;
using vela::runtime::Isolate;
using vela::runtime::RootRange;
using vela::runtime::Value;
using vela::vm::EncodeABC;
using vela::vm::EncodeAImm16;
using vela::vm::ExecutionStatus;
using vela::vm::Function;
using vela::vm::Instruction;
using vela::vm::OpCode;
using vela::vm::VirtualMachine;

bool TestIntegerExecution() {
  constexpr std::array<Instruction, 4> code = {
      EncodeAImm16(OpCode::LoadInt, 0, 20),
      EncodeAImm16(OpCode::LoadInt, 1, 22),
      EncodeABC(OpCode::AddInt, 2, 0, 1),
      EncodeABC(OpCode::Return, 2, 0, 0),
  };
  const Function function{.code = code, .classes = {}, .register_count = 3};

  Isolate isolate;
  VirtualMachine vm(isolate);
  const auto result = vm.Execute(function);
  return result.status == ExecutionStatus::Returned && result.value.IsInteger() &&
         result.value.AsInteger() == 42;
}

bool TestFrameRegistersStayRootedAcrossCollection() {
  Isolate isolate(64);
  constexpr std::array<std::uint32_t, 1> reference_fields = {0};
  auto* node_layout = isolate.CreateClassLayout(1, reference_fields);
  std::array<vela::runtime::ClassLayout*, 1> classes = {node_layout};
  const std::array<Instruction, 6> code = {
      EncodeABC(OpCode::NewInstance, 0, 0, 0), EncodeABC(OpCode::NewInstance, 1, 0, 0),
      EncodeABC(OpCode::SetField, 0, 0, 1),    EncodeABC(OpCode::NewInstance, 2, 0, 0),
      EncodeABC(OpCode::GetField, 3, 0, 0),    EncodeABC(OpCode::Return, 3, 0, 0),
  };
  const Function function{
      .code = code,
      .classes = std::span<vela::runtime::ClassLayout* const>(classes.data(), classes.size()),
      .register_count = 4};

  VirtualMachine vm(isolate);
  const auto result = vm.Execute(function);
  if (result.status != ExecutionStatus::Returned || !result.value.IsObject() ||
      result.value.AsObject() == nullptr || isolate.heap().object_count() != 3) {
    return false;
  }

  Value result_root = result.value;
  RootRange root(isolate.heap(), &result_root, 1);
  isolate.heap().Collect();
  if (isolate.heap().object_count() != 1 || isolate.heap().phase() != GcPhase::Idle) {
    return false;
  }

  result_root = Value::Null();
  isolate.heap().Collect();
  return isolate.heap().object_count() == 0;
}

bool TestInvalidBytecodeIsRejected() {
  constexpr std::array<Instruction, 1> code = {EncodeABC(static_cast<OpCode>(0xff), 0, 0, 0)};
  const Function function{.code = code, .classes = {}, .register_count = 1};

  Isolate isolate;
  VirtualMachine vm(isolate);
  return vm.Execute(function).status == ExecutionStatus::InvalidBytecode;
}

}  // namespace

int main() {
  if (!TestIntegerExecution() || !TestFrameRegistersStayRootedAcrossCollection() ||
      !TestInvalidBytecodeIsRejected()) {
    std::cerr << "vm tests failed\n";
    return 1;
  }
  return 0;
}
