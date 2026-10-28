#include "compiler/ir/analysis.h"

#include <algorithm>

#include <gtest/gtest.h>

#include "compiler/ir/ir.h"
#include "compiler/ir/optimizer.h"

namespace vela::ir {
namespace {

TEST(AnalysisTest, FailureKeepsPreviousDestinationAliveForTheHandler) {
  Function function;
  function.parameter_count = 2;
  function.register_count = 3;
  function.handlers = {{.target = 4}};
  function.instructions = {
      {.opcode = Opcode::LoadString, .dst = 0},
      {.opcode = Opcode::Jump, .index = 2},
      {.opcode = Opcode::CheckedCast, .dst = 0, .lhs = 1, .handler = 0},
      {.opcode = Opcode::Return, .dst = 0},
      {.opcode = Opcode::Catch, .dst = 2},
      {.opcode = Opcode::Return, .dst = 0},
  };
  const auto graph = BuildFlowGraph(function);
  const auto liveness = AnalyzeLiveness(function, graph, {});
  const auto& before_cast = liveness.at(graph.instruction_blocks.at(2)).live_in;
  EXPECT_TRUE(before_cast.contains(0));
  EXPECT_TRUE(before_cast.contains(1));

  Optimize(function, {});
  EXPECT_TRUE(std::ranges::any_of(function.instructions, [](const auto& instruction) {
    return instruction.opcode == Opcode::LoadString;
  }));
}

TEST(AnalysisTest, SuccessfulOverwriteDoesNotKeepThePreviousDestinationAlive) {
  Function function;
  function.parameter_count = 2;
  function.register_count = 2;
  function.instructions = {
      {.opcode = Opcode::LoadString, .dst = 0},
      {.opcode = Opcode::CheckedCast, .dst = 0, .lhs = 1},
      {.opcode = Opcode::Return, .dst = 0},
  };
  Optimize(function, {});
  EXPECT_FALSE(std::ranges::any_of(function.instructions, [](const auto& instruction) {
    return instruction.opcode == Opcode::LoadString;
  }));
}

TEST(AnalysisTest, AwaitHasASeparateResumeBlockWithoutAnExceptionHandler) {
  Function function;
  function.is_async = true;
  function.parameter_count = 1;
  function.register_count = 2;
  function.instructions = {
      {.opcode = Opcode::Await, .dst = 1, .lhs = 0},
      {.opcode = Opcode::Return, .dst = 1},
  };
  const auto graph = BuildFlowGraph(function);
  ASSERT_EQ(graph.blocks.size(), 2U);
  EXPECT_EQ(graph.blocks.front().end, 1U);
  ASSERT_EQ(graph.blocks.front().successors.size(), 1U);
  EXPECT_EQ(graph.blocks.front().successors.front(), 1U);
  const auto liveness = AnalyzeLiveness(function, graph, {});
  EXPECT_TRUE(liveness.front().live_in.contains(0));
  EXPECT_FALSE(liveness.front().live_in.contains(1));
  EXPECT_TRUE(liveness.front().live_out.contains(1));
}

TEST(AnalysisTest, ReturnCompletionRetainsItsExceptionHandler) {
  Function function;
  function.is_async = true;
  function.register_count = 2;
  function.handlers = {{.target = 2}};
  function.instructions = {
      {.opcode = Opcode::LoadInt, .dst = 0, .immediate = 1},
      {.opcode = Opcode::Return, .dst = 0, .handler = 0},
      {.opcode = Opcode::Catch, .dst = 1},
      {.opcode = Opcode::Return, .dst = 1},
  };
  Optimize(function, {});
  EXPECT_TRUE(std::ranges::any_of(function.instructions, [](const auto& instruction) {
    return instruction.opcode == Opcode::Catch;
  }));
}

}  // namespace
}  // namespace vela::ir
