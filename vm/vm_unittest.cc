#include <array>
#include <cstdint>

#include "gtest/gtest.h"

#include "vm/vm.h"

namespace vela::vm {
namespace {

constexpr std::int64_t kExpectedSum = 42;

using vela::runtime::GcPhase;
using vela::runtime::Isolate;
using vela::runtime::RootRange;
using vela::runtime::Value;
TEST(VmTest, IntegerExecution) {
  constexpr std::array<Instruction, 4> code = {
      EncodeAImm16(OpCode::LoadInt, 0, 20),
      EncodeAImm16(OpCode::LoadInt, 1, 22),
      EncodeABC(OpCode::AddInt, 2, 0, 1),
      EncodeABC(OpCode::Return, 2, 0, 0),
  };
  const Function function{
      .code = code,
      .classes = std::span<vela::runtime::ClassLayout* const>(),
      .strings = std::span<vela::runtime::StringObject* const>(),
      .register_count = 3,
  };

  Isolate isolate;
  VirtualMachine machine(isolate);
  const auto result = machine.Execute(function);
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kExpectedSum);
}

TEST(VmTest, FrameRegistersStayRootedAcrossCollection) {
  constexpr std::size_t kGcStressThreshold = 64;
  Isolate isolate(kGcStressThreshold);
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
      .strings = std::span<vela::runtime::StringObject* const>(),
      .register_count = 4,
  };

  VirtualMachine machine(isolate);
  const auto result = machine.Execute(function);
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsObject());
  ASSERT_NE(result.value.AsObject(), nullptr);
  EXPECT_EQ(isolate.heap().object_count(), 3U);

  Value result_root = result.value;
  RootRange root(isolate.heap(), &result_root, 1);
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 1U);
  EXPECT_EQ(isolate.heap().phase(), GcPhase::Idle);

  result_root = Value::Null();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(VmTest, InvalidBytecodeIsRejected) {
  constexpr std::array<Instruction, 1> code = {EncodeABC(static_cast<OpCode>(0xff), 0, 0, 0)};
  const Function function{
      .code = code,
      .classes = std::span<vela::runtime::ClassLayout* const>(),
      .strings = std::span<vela::runtime::StringObject* const>(),
      .register_count = 1,
  };

  Isolate isolate;
  VirtualMachine machine(isolate);
  EXPECT_EQ(machine.Execute(function).status, ExecutionStatus::InvalidBytecode);
}

TEST(VmTest, DivisionRejectsInvalidSourceRegister) {
  constexpr std::array<Instruction, 2> code = {
      EncodeABC(OpCode::DivInt, 0, 0, 1),
      EncodeABC(OpCode::Return, 0, 0, 0),
  };
  const Function function{
      .code = code,
      .classes = std::span<vela::runtime::ClassLayout* const>(),
      .strings = std::span<vela::runtime::StringObject* const>(),
      .register_count = 1,
  };
  Isolate isolate;
  EXPECT_EQ(VirtualMachine(isolate).Execute(function).status, ExecutionStatus::InvalidBytecode);
}

}  // namespace
}  // namespace vela::vm
