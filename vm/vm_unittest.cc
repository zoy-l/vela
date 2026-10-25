#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "base/builtin.h"
#include "gtest/gtest.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/bytecode.h"

#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::vm {
namespace {

constexpr std::int64_t kExpectedSum = 42;

using vela::runtime::GcPhase;
using vela::runtime::Isolate;
using vela::runtime::RootRange;
using vela::runtime::Value;

ExecutionResult LoadAndExecute(Isolate& isolate, const Function& function) {
  const std::array<Function, 1> functions = {function};
  auto program = Program::Load(functions, isolate);
  if (!program) {
    ADD_FAILURE() << program.error().message;
    return {};
  }
  return VirtualMachine(isolate).Execute(*program, 0);
}

TEST(VmTest, StringConcatSupportsAliasedDestinationsAcrossSafepoints) {
  Isolate isolate(1);
  std::array<runtime::StringObject*, 1> constants{};
  RootRange constant_roots(isolate.heap(), constants);
  constants.front() = isolate.heap().AllocateString("x");
  constexpr auto kCode = std::to_array<Instruction>({
      EncodeABC(OpCode::LoadString, 0, 0, 0),
      EncodeABC(OpCode::StringConcat, 0, 0, 0),
      EncodeABC(OpCode::StringConcat, 1, 0, 0),
      EncodeABC(OpCode::StringConcat, 0, 1, 0),
      EncodeABC(OpCode::StringConcat, 0, 0, 1),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  Function function;
  function.code = kCode;
  function.strings = constants;
  function.register_count = 2;
  const auto result = LoadAndExecute(isolate, function);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "xxxxxxxxxx");
}

TEST(VmTest, InterpolationAndBuilderAllowAliasedDestinationsAcrossSafepoints) {
  Isolate isolate(1);
  std::array<runtime::StringObject*, 1> constants{};
  RootRange roots(isolate.heap(), constants);
  constants.front() = isolate.heap().AllocateString("x");
  constexpr auto kCode = std::to_array<Instruction>({
      EncodeABC(OpCode::LoadString, 0, 0, 0),
      EncodeABC(OpCode::LoadString, 1, 0, 0),
      EncodeABC(OpCode::StringInterpolate, 0, 0, 2),
      EncodeABC(OpCode::BuiltinCall, 2, static_cast<std::uint8_t>(Builtin::NewStringBuilder), 0),
      EncodeABC(OpCode::Move, 3, 0, 0),
      EncodeABC(OpCode::BuiltinCall, 4, static_cast<std::uint8_t>(Builtin::BuilderAppend), 2),
      EncodeABC(OpCode::BuiltinCall, 2, static_cast<std::uint8_t>(Builtin::BuilderToString), 2),
      EncodeABC(OpCode::Return, 2, 0, 0),
  });
  Function function;
  function.code = kCode;
  function.strings = constants;
  constexpr std::uint16_t kRequiredRegisters = 5;
  function.register_count = kRequiredRegisters;
  const auto result = LoadAndExecute(isolate, function);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "xx");
}

TEST(VmTest, JoinAllowsDestinationToOverwriteRootedInputArray) {
  Isolate isolate(1);
  std::array<runtime::StringObject*, 1> constants{};
  RootRange roots(isolate.heap(), constants);
  constants.front() = isolate.heap().AllocateString("x");
  const auto* layout = isolate.CreateArrayLayout({.element_kind = runtime::ValueKind::String});
  const std::array<const runtime::ArrayLayout*, 1> arrays = {layout};
  constexpr auto kCode = std::to_array<Instruction>({
      EncodeABC(OpCode::NewArray, 0, 0, 2),
      EncodeABC(OpCode::LoadString, 1, 0, 0),
      EncodeAImm16(OpCode::LoadInt, 2, 0),
      EncodeABC(OpCode::SetIndex, 0, 2, 1),
      EncodeAImm16(OpCode::LoadInt, 2, 1),
      EncodeABC(OpCode::SetIndex, 0, 2, 1),
      EncodeABC(OpCode::BuiltinCall, 0, static_cast<std::uint8_t>(Builtin::ArrayJoin), 0),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  Function function;
  function.code = kCode;
  function.strings = constants;
  function.arrays = arrays;
  function.register_count = 3;
  const auto result = LoadAndExecute(isolate, function);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "xxx");
}

class StringBytecodeValidationTest : public testing::TestWithParam<Instruction> {};

TEST_P(StringBytecodeValidationTest, RejectsInvalidBuiltinIdsAndArgumentRanges) {
  Isolate isolate;
  const std::array<Instruction, 2> code = {GetParam(), EncodeABC(OpCode::Return, 0, 0, 0)};
  Function function;
  function.code = code;
  function.register_count = 2;
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
}

INSTANTIATE_TEST_SUITE_P(
    InvalidCalls,
    StringBytecodeValidationTest,
    testing::Values(
        EncodeABC(OpCode::BuiltinCall, 0, static_cast<std::uint8_t>(Builtin::Count), 0),
        EncodeABC(OpCode::BuiltinCall, 0, static_cast<std::uint8_t>(Builtin::ArrayJoin), 1),
        EncodeABC(OpCode::StringInterpolate, 0, 1, 2)));

TEST(VmTest, IntegerExecution) {
  constexpr std::array<Instruction, 4> code = {
      EncodeAImm16(OpCode::LoadInt, 0, 20),
      EncodeAImm16(OpCode::LoadInt, 1, 22),
      EncodeABC(OpCode::AddInt, 2, 0, 1),
      EncodeABC(OpCode::Return, 2, 0, 0),
  };
  Function function;
  function.code = code;
  function.classes = std::span<vela::runtime::ClassLayout* const>();
  function.strings = std::span<vela::runtime::StringObject* const>();
  function.register_count = 3;

  Isolate isolate;
  const auto result = LoadAndExecute(isolate, function);
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
  Function function;
  function.code = code;
  function.classes = std::span<vela::runtime::ClassLayout* const>(classes.data(), classes.size());
  function.strings = std::span<vela::runtime::StringObject* const>();
  function.register_count = 4;

  const auto result = LoadAndExecute(isolate, function);
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

TEST(VmTest, RejectsUnknownOpcodeDuringLoading) {
  constexpr std::array<Instruction, 1> code = {EncodeABC(static_cast<OpCode>(0xff), 0, 0, 0)};
  Function function;
  function.code = code;
  function.classes = std::span<vela::runtime::ClassLayout* const>();
  function.strings = std::span<vela::runtime::StringObject* const>();
  function.register_count = 1;

  Isolate isolate;
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
}

TEST(VmTest, DivisionRejectsInvalidSourceRegister) {
  constexpr std::array<Instruction, 2> code = {
      EncodeABC(OpCode::DivInt, 0, 0, 1),
      EncodeABC(OpCode::Return, 0, 0, 0),
  };
  Function function;
  function.code = code;
  function.classes = std::span<vela::runtime::ClassLayout* const>();
  function.strings = std::span<vela::runtime::StringObject* const>();
  function.register_count = 1;
  Isolate isolate;
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
}

TEST(VmTest, RejectsInvalidArrayLayoutIndices) {
  runtime::Isolate isolate;
  const auto* layout = isolate.CreateArrayLayout({.element_kind = runtime::ValueKind::Integer});
  const std::array<const runtime::ArrayLayout*, 1> layouts = {layout};
  const auto code = std::to_array<Instruction>({
      EncodeABC(OpCode::NewArray, 0, 1, 1),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  Function function;
  function.code = code;
  function.arrays = layouts;
  function.register_count = 1;
  function.parameter_count = 0;
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
  const std::array<const runtime::ArrayLayout*, 1> null_layout = {nullptr};
  const auto valid_code = std::to_array<Instruction>({
      EncodeABC(OpCode::NewArray, 0, 0, 1),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  function.code = valid_code;
  function.arrays = null_layout;
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
}

TEST(VmTest, RejectsWrongArrayStoreAndNonArrayIndexing) {
  runtime::Isolate isolate;
  const auto* layout = isolate.CreateArrayLayout({.element_kind = runtime::ValueKind::Integer});
  const std::array<const runtime::ArrayLayout*, 1> layouts = {layout};
  const auto code = std::to_array<Instruction>({
      EncodeABC(OpCode::NewArray, 0, 0, 1),
      EncodeAImm16(OpCode::LoadInt, 1, 0),
      EncodeABC(OpCode::LoadBool, 2, 1, 0),
      EncodeABC(OpCode::SetIndex, 0, 1, 2),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  Function function;
  function.code = code;
  function.arrays = layouts;
  function.register_count = 3;
  function.parameter_count = 0;
  EXPECT_EQ(LoadAndExecute(isolate, function).status, ExecutionStatus::TypeError);
  auto* record = isolate.CreateClassLayout(0, {});
  const std::array<runtime::ClassLayout*, 1> classes = {record};
  const auto invalid_access = std::to_array<Instruction>({
      EncodeABC(OpCode::NewInstance, 0, 0, 0),
      EncodeAImm16(OpCode::LoadInt, 1, 0),
      EncodeABC(OpCode::GetIndex, 2, 0, 1),
      EncodeABC(OpCode::Return, 2, 0, 0),
  });
  function.code = invalid_access;
  function.classes = classes;
  EXPECT_EQ(LoadAndExecute(isolate, function).status, ExecutionStatus::TypeError);
}

TEST(VmTest, ValidatesDoubleConstantAndRegisterOperands) {
  const std::array<double, 1> constants = {1.5};
  auto code = std::to_array<Instruction>({
      EncodeABC(OpCode::LoadDouble, 0, 1, 0),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  Function function;
  function.code = code;
  function.doubles = constants;
  function.register_count = 1;
  function.parameter_count = 0;
  Isolate isolate;
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
  code.front() = EncodeABC(OpCode::LoadDouble, 1, 0, 0);
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
  code.front() = EncodeABC(OpCode::LoadDouble, 0, 0, 0);
  const auto result = LoadAndExecute(isolate, function);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_DOUBLE_EQ(result.value.AsDouble(), constants.front());
}

class VmDoubleOperationTest : public testing::TestWithParam<OpCode> {};

TEST_P(VmDoubleOperationTest, RejectsIntegerValuesAndInvalidRegisters) {
  const std::array<double, 1> constants = {1.5};
  auto code = std::to_array<Instruction>({
      EncodeABC(OpCode::LoadDouble, 0, 0, 0),
      EncodeAImm16(OpCode::LoadInt, 1, 1),
      EncodeABC(OpCode::AddDouble, 2, 0, 1),
      EncodeABC(OpCode::Return, 2, 0, 0),
  });
  Function function;
  function.code = code;
  function.doubles = constants;
  function.register_count = 3;
  function.parameter_count = 0;
  Isolate isolate;
  const auto opcode = GetParam();
  code.at(2) = EncodeABC(opcode, 2, 0, 1);
  EXPECT_EQ(LoadAndExecute(isolate, function).status, ExecutionStatus::TypeError);
  code.at(2) = EncodeABC(opcode, 2, 3, 0);
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
  code.at(2) = EncodeABC(opcode, 2, 0, 3);
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
  code.at(2) = EncodeABC(opcode, 3, 0, 0);
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
}

INSTANTIATE_TEST_SUITE_P(ArithmeticAndComparisons,
                         VmDoubleOperationTest,
                         testing::Values(OpCode::AddDouble,
                                         OpCode::SubDouble,
                                         OpCode::MulDouble,
                                         OpCode::DivDouble,
                                         OpCode::EqualDouble,
                                         OpCode::NotEqualDouble,
                                         OpCode::LessDouble,
                                         OpCode::LessEqualDouble,
                                         OpCode::GreaterDouble,
                                         OpCode::GreaterEqualDouble));

TEST(VmTest, DoubleNegationRejectsIntegerAndInvalidRegister) {
  auto code = std::to_array<Instruction>({
      EncodeAImm16(OpCode::LoadInt, 0, 1),
      EncodeABC(OpCode::NegDouble, 1, 0, 0),
      EncodeABC(OpCode::Return, 1, 0, 0),
  });
  Function function;
  function.code = code;
  function.register_count = 2;
  function.parameter_count = 0;
  Isolate isolate;
  EXPECT_EQ(LoadAndExecute(isolate, function).status, ExecutionStatus::TypeError);
  code.at(1) = EncodeABC(OpCode::NegDouble, 1, 2, 0);
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate).has_value());
}

}  // namespace
}  // namespace vela::vm
