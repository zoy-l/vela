#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "base/callable.h"
#include "base/execution_limits.h"

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/register_storage.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/debug_info.h"
#include "vm/exception_handler.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::vm {
namespace {

constexpr std::int64_t kExpectedResult = 42;
constexpr auto kInvalidOpcode = static_cast<OpCode>(0xff);
constexpr std::int16_t kOutsideJump = 5;
constexpr std::uint32_t kOriginalLine = 7;
constexpr std::uint32_t kChangedLine = 99;

TEST(ProgramTest, ValidatesSourceMappingBoundariesAndOrder) {
  runtime::Isolate isolate;
  const auto code = std::to_array<Instruction>(
      {EncodeAImm16(OpCode::LoadInt, 0, 0), EncodeABC(OpCode::Return, 0, 0, 0)});
  const std::array<std::string, 1> paths = {"main.vela"};
  auto mappings = std::to_array<SourceMapping>({
      {.instruction = 0, .location = {.line = 1}},
      {.instruction = 1, .location = {.line = 2}},
  });
  Function input;
  input.code = code;
  input.register_count = 1;
  input.debug = {.name = "main", .source_paths = paths, .mappings = mappings};
  EXPECT_TRUE(Program::Load(std::span(&input, 1), isolate));
  mappings.back().instruction = 0;
  EXPECT_FALSE(Program::Load(std::span(&input, 1), isolate));
  mappings.back().instruction = 2;
  EXPECT_FALSE(Program::Load(std::span(&input, 1), isolate));
  mappings.back().instruction = 1;
  mappings.back().location.source_id = 1;
  EXPECT_FALSE(Program::Load(std::span(&input, 1), isolate));
  mappings.back().location.source_id = 0;
  mappings.front().instruction = 1;
  EXPECT_FALSE(Program::Load(std::span(&input, 1), isolate));
}

TEST(ProgramTest, LoadsUnsigned16IntegerAndDoubleConstantIndicesAndSnapshotsPools) {
  runtime::Isolate isolate;
  constexpr auto kLastIndex = std::numeric_limits<std::uint16_t>::max();
  constexpr std::int64_t kLargeInteger = std::numeric_limits<std::int64_t>::max();
  constexpr double kLargeDouble = 1e100;
  std::vector<std::int64_t> integers(static_cast<std::size_t>(kLastIndex) + 1);
  std::vector<double> doubles(integers.size());
  integers.back() = kLargeInteger;
  doubles.back() = kLargeDouble;
  const auto code = std::to_array<Instruction>({
      EncodeAUnsigned16(OpCode::LoadIntConstant, 0, kLastIndex),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  Function function;
  function.code = code;
  function.integers = integers;
  function.register_count = 1;
  const auto program = Program::Load(std::span(&function, 1), isolate);
  ASSERT_TRUE(program);
  integers.back() = 0;
  EXPECT_EQ(VirtualMachine(isolate).Execute(*program, 0).value.AsInteger(), kLargeInteger);
  const auto floating_code = std::to_array<Instruction>({
      EncodeAUnsigned16(OpCode::LoadDouble, 0, kLastIndex),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  function.code = floating_code;
  function.doubles = doubles;
  const auto floating = Program::Load(std::span(&function, 1), isolate);
  ASSERT_TRUE(floating);
  EXPECT_EQ(VirtualMachine(isolate).Execute(*floating, 0).value.AsDouble(), kLargeDouble);
  function.doubles = std::span(doubles).first(kLastIndex);
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate));
}

TEST(ProgramTest, ValidatesExceptionRangesAndCatchTargetsBeforeExecution) {
  runtime::Isolate isolate;
  const auto code = std::to_array<Instruction>({
      EncodeAImm16(OpCode::LoadInt, 0, 0),
      EncodeABC(OpCode::DivInt, 0, 0, 0),
      EncodeABC(OpCode::Return, 0, 0, 0),
      EncodeABC(OpCode::Catch, 0, 0, 0),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  auto handlers = std::to_array<ExceptionHandler>({{.begin = 1, .end = 2, .target = 3}});
  Function function;
  function.code = code;
  function.register_count = 1;
  function.handlers = handlers;
  EXPECT_TRUE(Program::Load(std::span(&function, 1), isolate));
  handlers.front().target = 2;
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate));
  handlers.front().target = 3;
  handlers.front().end = 1;
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate));
  handlers.front().end = static_cast<std::uint32_t>(code.size() + 1);
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate));
  handlers.front().end = 2;
  const std::array<ExceptionHandler, 2> overlapping = {handlers.front(), handlers.front()};
  function.handlers = overlapping;
  EXPECT_FALSE(Program::Load(std::span(&function, 1), isolate));
}

TEST(ProgramTest, LoadsOwnedDebugInfoAndTracesBytecodeWithoutSymbols) {
  runtime::Isolate isolate;
  const auto code = std::to_array<Instruction>({
      EncodeAImm16(OpCode::LoadInt, 0, 0),
      EncodeABC(OpCode::DivInt, 0, 0, 0),
      EncodeABC(OpCode::Return, 0, 0, 0),
  });
  std::array<std::string, 1> paths = {"original.vela"};
  std::array<SourceMapping, 1> mappings = {
      {{.instruction = 0, .location = {.line = kOriginalLine}}},
  };
  Function input;
  input.code = code;
  input.register_count = 1;
  input.debug = {.name = "divide", .source_paths = paths, .mappings = mappings};
  auto program = Program::Load(std::span(&input, 1), isolate);
  ASSERT_TRUE(program);
  paths.front() = "changed.vela";
  mappings.front().location.line = kChangedLine;
  const auto result = VirtualMachine(isolate).Execute(*program, 0);
  ASSERT_EQ(result.stack_trace.size(), 1U);
  EXPECT_EQ(result.stack_trace.front().source_path, "original.vela");
  EXPECT_EQ(result.stack_trace.front().location.line, 7U);
  input.debug = {};
  program = Program::Load(std::span(&input, 1), isolate);
  ASSERT_TRUE(program);
  const auto anonymous = VirtualMachine(isolate).Execute(*program, 0);
  std::ostringstream output;
  WriteRuntimeError(output, anonymous);
  EXPECT_EQ(output.str(), "vela: error: division by zero\n  at <anonymous> (instruction 1)\n");
}

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
  EXPECT_EQ(machine.register_bytes(), runtime::kRegisterSlotBytes);
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

TEST(ProgramTest, RejectsInvalidCallableAndInterfaceTargetsAtLoading) {
  runtime::Isolate isolate;
  const auto code = std::to_array<Instruction>({EncodeABC(OpCode::Return, 0, 0, 0)});
  const auto layouts = std::to_array<runtime::ClassLayoutOptions>({
      {
          .callable = CallableTarget{.function = 1, .mode = CallableMode::Plain},
          .interfaces = {},
          .dispatch_targets = {},
      },
      {
          .callable = CallableTarget{.function = 0, .mode = CallableMode::Closure},
          .interfaces = {},
          .dispatch_targets = {},
      },
      {
          .callable = CallableTarget{.function = 0, .mode = CallableMode::InterfaceMethod},
          .interfaces = {},
          .dispatch_targets = {},
      },
      {
          .callable = CallableTarget{.function = 0, .mode = static_cast<CallableMode>(0xff)},
          .interfaces = {},
          .dispatch_targets = {},
      },
      {.callable = std::nullopt, .interfaces = {}, .dispatch_targets = {0}},
  });
  for (const auto& options : layouts) {
    auto* layout = isolate.CreateClassLayout(0, {}, options);
    Function input;
    input.code = code;
    input.register_count = 1;
    input.classes = std::span(&layout, 1);
    EXPECT_FALSE(Program::Load(std::span(&input, 1), isolate).has_value());
  }
}

TEST(ProgramTest, SnapshotsAndSharesLayoutTablesAcrossFunctions) {
  runtime::Isolate isolate;
  auto* original = isolate.CreateClassLayout(0, {});
  auto* replacement = isolate.CreateClassLayout(1, {});
  std::array<runtime::ClassLayout*, 1> classes = {original};
  const auto code = std::to_array<Instruction>(
      {EncodeABC(OpCode::NewInstance, 0, 0, 0), EncodeABC(OpCode::Return, 0, 0, 0)});
  Function input;
  input.code = code;
  input.classes = classes;
  input.register_count = 1;
  const std::array<Function, 2> functions = {input, input};
  auto program = Program::Load(functions, isolate);
  ASSERT_TRUE(program.has_value());
  const auto stored = program->functions();
  EXPECT_EQ(stored.front().layouts, stored.back().layouts);
  classes.front() = replacement;
  EXPECT_EQ(stored.front().View().classes.front(), original);
  VirtualMachine machine(isolate);
  EXPECT_TRUE(machine.Execute(*program, 0).Succeeded());
  EXPECT_TRUE(machine.Execute(*program, 1).Succeeded());
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
  EXPECT_EQ(machine.register_bytes(), kMaxFrameRegisters * runtime::kRegisterSlotBytes);
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
  EXPECT_EQ(bytes, 3U * runtime::kRegisterSlotBytes);
  EXPECT_TRUE(machine.Execute(*program, 0).Succeeded());
  EXPECT_EQ(machine.register_bytes(), bytes);
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

}  // namespace
}  // namespace vela::vm
