#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "base/execution_limits.h"

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::vm {
namespace {

constexpr std::int64_t kExpectedResult = 42;
constexpr auto kInvalidOpcode = static_cast<OpCode>(0xff);
constexpr std::int16_t kOutsideJump = 5;

TEST(ProgramTest, SnapshotsMutableInputsBeforeValidationAndExecution) {
  runtime::Isolate isolate;
  auto code = std::to_array<Instruction>(
      {EncodeAImm16(OpCode::LoadInt, 0, kExpectedResult), EncodeABC(OpCode::Return, 0, 0, 0)});
  Function input;
  input.code = code;
  input.register_count = 1;
  const std::array<Function, 1> functions = {input};
  auto program = Program::Load(functions, isolate);
  ASSERT_TRUE(program.has_value());
  code.front() = EncodeABC(kInvalidOpcode, 0, 0, 0);
  VirtualMachine machine(isolate);
  const auto first = machine.Execute(*program, 0);
  const auto second = machine.Execute(*program, 0);
  ASSERT_TRUE(first.Succeeded());
  ASSERT_TRUE(second.Succeeded());
  EXPECT_EQ(first.value.AsInteger(), kExpectedResult);
  EXPECT_EQ(second.value.AsInteger(), kExpectedResult);
  EXPECT_EQ(machine.register_bytes(), sizeof(runtime::Value));
}

TEST(ProgramTest, RejectsInvalidJumpsAndFallthroughAtLoading) {
  runtime::Isolate isolate;
  auto code = std::to_array<Instruction>(
      {EncodeAImm16(OpCode::Jump, 0, kOutsideJump), EncodeABC(OpCode::Return, 0, 0, 0)});
  Function input;
  input.code = code;
  input.register_count = 1;
  const std::array<Function, 1> functions = {input};
  EXPECT_FALSE(Program::Load(functions, isolate).has_value());
  code.front() = EncodeAImm16(OpCode::Jump, 0, -2);
  EXPECT_FALSE(Program::Load(functions, isolate).has_value());
  code.front() = EncodeAImm16(OpCode::LoadInt, 0, 1);
  code.back() = EncodeAImm16(OpCode::LoadInt, 0, 2);
  EXPECT_FALSE(Program::Load(functions, isolate).has_value());
}

TEST(ProgramTest, AcceptsAll256RegisterSlotsAndRejects257) {
  runtime::Isolate isolate;
  const auto code = std::to_array<Instruction>(
      {EncodeAImm16(OpCode::LoadInt, 255, kExpectedResult), EncodeABC(OpCode::Return, 255, 0, 0)});
  Function input;
  input.code = code;
  input.register_count = kMaxFrameRegisters;
  auto program = Program::Load(std::span(&input, 1), isolate);
  ASSERT_TRUE(program.has_value());
  VirtualMachine machine(isolate);
  const auto result = machine.Execute(*program, 0);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), kExpectedResult);
  EXPECT_EQ(machine.register_bytes(), kMaxFrameRegisters * sizeof(runtime::Value));
  input.register_count = kMaxFrameRegisters + 1;
  EXPECT_FALSE(Program::Load(std::span(&input, 1), isolate).has_value());
}

TEST(ProgramTest, RejectsAnEntryFromAnotherIsolate) {
  runtime::Isolate first;
  runtime::Isolate second;
  const auto code = std::to_array<Instruction>({EncodeABC(OpCode::Return, 0, 0, 0)});
  Function input;
  input.code = code;
  input.register_count = 1;
  auto program = Program::Load(std::span(&input, 1), first);
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(VirtualMachine(second).Execute(*program, 0).status, ExecutionStatus::InvalidIsolate);
}

TEST(ProgramTest, AllocatesAndReusesOnlyTheNeededCallDepthStorage) {
  runtime::Isolate isolate;
  std::vector<FunctionStorage> functions;
  FunctionStorage entry;
  entry.name = "main";
  entry.register_count = 1;
  entry.code = {EncodeABC(OpCode::Call, 0, 1, 0), EncodeABC(OpCode::Return, 0, 0, 0)};
  functions.push_back(std::move(entry));
  FunctionStorage callee;
  callee.name = "callee";
  callee.register_count = 2;
  callee.code = {
      EncodeAImm16(OpCode::LoadInt, 1, kExpectedResult),
      EncodeABC(OpCode::Return, 1, 0, 0),
  };
  functions.push_back(std::move(callee));
  auto program = Program::Create(std::move(functions), isolate);
  ASSERT_TRUE(program.has_value());
  VirtualMachine machine(isolate);
  EXPECT_TRUE(machine.Execute(*program, 0).Succeeded());
  const auto bytes = machine.register_bytes();
  EXPECT_EQ(bytes, 3U * sizeof(runtime::Value));
  EXPECT_TRUE(machine.Execute(*program, 0).Succeeded());
  EXPECT_EQ(machine.register_bytes(), bytes);
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

}  // namespace
}  // namespace vela::vm
