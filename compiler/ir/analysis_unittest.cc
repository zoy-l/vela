#include "compiler/ir/analysis.h"

#include <algorithm>
#include <vector>

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
      {.opcode = Opcode::LoadString, .dst = 0, .location = {}},
      {.opcode = Opcode::Jump, .index = 2, .location = {}},
      {.opcode = Opcode::CheckedCast, .dst = 0, .lhs = 1, .location = {}, .handler = 0},
      {.opcode = Opcode::Return, .dst = 0, .location = {}},
      {.opcode = Opcode::Catch, .dst = 2, .location = {}},
      {.opcode = Opcode::Return, .dst = 0, .location = {}},
  };
  const auto graph = BuildFlowGraph(function);
  const auto liveness = AnalyzeLiveness(function, graph, {});
  const auto& before_cast = liveness.at(graph.instruction_blocks.at(2)).live_in;
  EXPECT_TRUE(before_cast.Has(0));
  EXPECT_TRUE(before_cast.Has(1));

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
      {.opcode = Opcode::LoadString, .dst = 0, .location = {}},
      {.opcode = Opcode::CheckedCast, .dst = 0, .lhs = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 0, .location = {}},
  };
  Optimize(function, {});
  EXPECT_FALSE(std::ranges::any_of(function.instructions, [](const auto& instruction) {
    return instruction.opcode == Opcode::LoadString;
  }));
}

TEST(AnalysisTest, CopyRemovalAndConstantBranchesShareStableInstructionPositions) {
  Function function;
  function.register_count = 3;
  function.instructions = {
      {.opcode = Opcode::LoadInt, .dst = 0, .immediate = 42, .location = {}},
      {.opcode = Opcode::Move, .dst = 1, .lhs = 0, .location = {}},
      {.opcode = Opcode::Move, .dst = 1, .lhs = 0, .location = {}},
      {.opcode = Opcode::LoadBool, .dst = 2, .immediate = 1, .location = {}},
      {.opcode = Opcode::JumpIfFalse, .lhs = 2, .index = 6, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
      {.opcode = Opcode::LoadInt, .dst = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
  };
  Optimize(function, {});
  ASSERT_EQ(function.instructions.size(), 2U);
  EXPECT_EQ(function.instructions.front().opcode, Opcode::LoadInt);
  EXPECT_EQ(function.instructions.front().immediate, 42);
  EXPECT_EQ(function.instructions.back().opcode, Opcode::Return);
  EXPECT_EQ(function.instructions.back().dst, function.instructions.front().dst);
}

TEST(AnalysisTest, AwaitHasASeparateResumeBlockWithoutAnExceptionHandler) {
  Function function;
  function.is_async = true;
  function.parameter_count = 1;
  function.register_count = 2;
  function.instructions = {
      {.opcode = Opcode::Await, .dst = 1, .lhs = 0, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
  };
  const auto graph = BuildFlowGraph(function);
  ASSERT_EQ(graph.blocks.size(), 2U);
  EXPECT_EQ(graph.blocks.front().end, 1U);
  ASSERT_EQ(graph.blocks.front().successors.size(), 1U);
  EXPECT_EQ(graph.blocks.front().successors.front(), 1U);
  const auto liveness = AnalyzeLiveness(function, graph, {});
  EXPECT_TRUE(liveness.front().live_in.Has(0));
  EXPECT_FALSE(liveness.front().live_in.Has(1));
  EXPECT_TRUE(liveness.front().live_out.Has(1));
}

TEST(AnalysisTest, ReturnCompletionRetainsItsExceptionHandler) {
  Function function;
  function.is_async = true;
  function.register_count = 2;
  function.handlers = {{.target = 2}};
  function.instructions = {
      {.opcode = Opcode::LoadInt, .dst = 0, .immediate = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 0, .location = {}, .handler = 0},
      {.opcode = Opcode::Catch, .dst = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
  };
  Optimize(function, {});
  EXPECT_TRUE(std::ranges::any_of(function.instructions, [](const auto& instruction) {
    return instruction.opcode == Opcode::Catch;
  }));
}

TEST(AnalysisTest, PropagatesLoopAndExceptionLivenessAcrossBitsetWords) {
  Function function;
  function.register_count = 258;
  function.handlers = {{.target = 6}};
  function.instructions = {
      {.opcode = Opcode::Jump, .index = 1, .location = {}},
      {.opcode = Opcode::JumpIfFalse, .lhs = 64, .index = 5, .location = {}},
      {.opcode = Opcode::CheckedCast, .dst = 257, .lhs = 63, .location = {}, .handler = 0},
      {.opcode = Opcode::Move, .dst = 63, .lhs = 256, .location = {}},
      {.opcode = Opcode::Jump, .index = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 257, .location = {}},
      {.opcode = Opcode::Catch, .dst = 65, .location = {}},
      {.opcode = Opcode::Return, .dst = 257, .location = {}},
  };
  const auto graph = BuildFlowGraph(function);
  const auto live = AnalyzeLiveness(function, graph, {});
  std::vector<RegisterId> entry;
  live.front().live_in.ForEach([&](RegisterId reg) { entry.push_back(reg); });
  EXPECT_EQ(entry, (std::vector<RegisterId>{63, 64, 256, 257}));
  const auto& before_cast = live.at(graph.instruction_blocks.at(2)).live_in;
  EXPECT_TRUE(before_cast.Has(63));
  EXPECT_TRUE(before_cast.Has(257));
  EXPECT_FALSE(before_cast.Has(65));
}

TEST(AnalysisTest, ExceptionalTransferKillsOnlyDefinitionsCompletedBeforeFailure) {
  Function function;
  function.register_count = 5;
  function.handlers = {{.target = 4}};
  function.instructions = {
      {.opcode = Opcode::LoadInt, .dst = 0, .immediate = 42, .location = {}, .handler = 0},
      {.opcode = Opcode::Move, .dst = 1, .lhs = 2, .location = {}, .handler = 0},
      {.opcode = Opcode::CheckedCast, .dst = 0, .lhs = 3, .location = {}, .handler = 0},
      {.opcode = Opcode::Return, .dst = 0, .location = {}},
      {.opcode = Opcode::Catch, .dst = 4, .location = {}},
      {.opcode = Opcode::Return, .dst = 0, .location = {}},
  };
  const auto graph = BuildFlowGraph(function);
  ASSERT_EQ(graph.blocks.front().end, 3U);
  const auto all = AnalyzeLiveness(function, graph, {});
  EXPECT_FALSE(all.front().live_in.Has(0));
  EXPECT_TRUE(all.front().live_in.Has(2));
  EXPECT_TRUE(all.front().live_in.Has(3));
  const auto necessary = AnalyzeNecessaryLiveness(function, graph, {});
  EXPECT_FALSE(necessary.blocks.front().live_in.Has(0));
  EXPECT_FALSE(necessary.blocks.front().live_in.Has(2));
  EXPECT_TRUE(necessary.blocks.front().live_in.Has(3));
  EXPECT_EQ(necessary.keep, (std::vector<std::uint8_t>{1, 0, 1, 1, 1, 1}));
}

TEST(AnalysisTest, NecessaryInstructionsBecomeLiveThroughLoopSuccessors) {
  Function function;
  function.register_count = 4;
  function.instructions = {
      {.opcode = Opcode::Move, .dst = 1, .lhs = 2, .location = {}},
      {.opcode = Opcode::Move, .dst = 3, .lhs = 2, .location = {}},
      {.opcode = Opcode::JumpIfFalse, .lhs = 0, .index = 0, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
  };
  const auto graph = BuildFlowGraph(function);
  const auto necessary = AnalyzeNecessaryLiveness(function, graph, {});
  EXPECT_TRUE(necessary.blocks.front().live_in.Has(0));
  EXPECT_TRUE(necessary.blocks.front().live_in.Has(2));
  EXPECT_FALSE(necessary.blocks.front().live_in.Has(1));
  EXPECT_FALSE(necessary.blocks.front().live_in.Has(3));
  EXPECT_EQ(necessary.keep, (std::vector<std::uint8_t>{1, 0, 1, 1}));
}

}  // namespace
}  // namespace vela::ir
