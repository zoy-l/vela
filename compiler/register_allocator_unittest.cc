#include "compiler/register_allocator.h"

#include <gtest/gtest.h>

#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::compiler {
namespace {

using Kind = frontend::TypeDescriptor::Kind;

ir::Function AwaitWithDeadParameter(frontend::TypeDescriptor type) {
  ir::Function function;
  function.is_async = true;
  function.parameter_count = 2;
  function.register_count = 3;
  function.value_types = {
      type, {.kind = Kind::Future, .name = "Future<int>"}, {.kind = Kind::Int, .name = "int"}};
  function.instructions = {
      {.opcode = Opcode::Await, .dst = 2, .lhs = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 2, .location = {}},
  };
  return function;
}

TEST(RegisterAllocationTest, DoesNotClearDeadPrimitiveSlotsBeforeAwait) {
  for (const auto kind : {Kind::Int, Kind::Double, Kind::Bool}) {
    auto function = AwaitWithDeadParameter({.kind = kind, .name = {}, .nullable = true});
    ASSERT_FALSE(AllocateRegisters(function, {}).has_value());
    EXPECT_EQ(function.register_count, 2U);
    ASSERT_EQ(function.instructions.size(), 2U);
    EXPECT_EQ(function.instructions.front().opcode, ir::Opcode::Await);
    EXPECT_TRUE(function.value_types.empty());
  }
}

TEST(RegisterAllocationTest, ClearsReferencesEvenWhenAnAwaitResultReusesTheirPhysicalSlot) {
  for (const auto kind : {Kind::String, Kind::Dynamic, Kind::Class, Kind::Enum, Kind::Function,
                          Kind::Array, Kind::Future, Kind::Map}) {
    auto function = AwaitWithDeadParameter({.kind = kind, .name = {}});
    ASSERT_FALSE(AllocateRegisters(function, {}).has_value());
    ASSERT_EQ(function.instructions.size(), 3U);
    const auto& clear = function.instructions[0];
    const auto& await = function.instructions[1];
    EXPECT_EQ(clear.opcode, ir::Opcode::LoadNull);
    EXPECT_EQ(await.opcode, ir::Opcode::Await);
    EXPECT_EQ(clear.dst, await.dst);
    EXPECT_NE(clear.dst, await.lhs);
    EXPECT_TRUE(function.value_types.empty());
  }
}

TEST(RegisterAllocationTest, ClearsAtEachSuspensionWithoutDiscardingLaterUses) {
  ir::Function function;
  function.is_async = true;
  function.parameter_count = 4;
  function.register_count = 6;
  function.value_types = {
      {.kind = Kind::String, .name = "string"},
      {.kind = Kind::String, .name = "string"},
      {.kind = Kind::Future, .name = "Future<int>"},
      {.kind = Kind::Future, .name = "Future<int>"},
      {.kind = Kind::Int, .name = "int"},
      {.kind = Kind::Int, .name = "int"},
  };
  function.instructions = {
      {.opcode = Opcode::Await, .dst = 4, .lhs = 2, .location = {}},
      {.opcode = Opcode::Print, .lhs = 0, .location = {}},
      {.opcode = Opcode::Await, .dst = 5, .lhs = 3, .location = {}},
      {.opcode = Opcode::Print, .lhs = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 5, .location = {}},
  };
  ASSERT_FALSE(AllocateRegisters(function, {}).has_value());
  ASSERT_EQ(function.instructions.size(), 7U);
  EXPECT_EQ(function.instructions[0].opcode, ir::Opcode::Await);
  EXPECT_EQ(function.instructions[1].opcode, ir::Opcode::Print);
  const auto first_reference = function.instructions[1].lhs;
  const auto second_reference = function.instructions[5].lhs;
  for (const auto index : {2U, 3U}) {
    const auto& clear = function.instructions[index];
    EXPECT_EQ(clear.opcode, ir::Opcode::LoadNull);
    EXPECT_NE(clear.dst, second_reference);
  }
  EXPECT_EQ(function.instructions[2].dst, first_reference);
  EXPECT_EQ(function.instructions[4].opcode, ir::Opcode::Await);
}

}  // namespace
}  // namespace vela::compiler
