#include "compiler/ir/analysis.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "compiler/ir/ir.h"
#include "compiler/ir/optimizer.h"

namespace vela::ir {
namespace {

TEST(AnalysisTest, InvalidIntegerConstantsKeepTheirOperationsAndExceptionEdges) {
  constexpr auto kMinimum = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
  struct Case final {
    Opcode opcode;
    std::int64_t left;
    std::int64_t right;
  };
  constexpr std::array<Case, 6> kCases = {{
      {Opcode::AddInt, kMaximum, 1},
      {Opcode::SubInt, kMinimum, 1},
      {Opcode::MulInt, kMaximum, 2},
      {Opcode::DivInt, kMinimum, -1},
      {Opcode::DivInt, 42, 0},
      {Opcode::NegInt, kMinimum, 0},
  }};
  for (const auto& scenario : kCases) {
    SCOPED_TRACE(static_cast<int>(scenario.opcode));
    Function function;
    function.register_count = 4;
    function.handlers = {{.target = 4}};
    function.instructions = {
        {.opcode = Opcode::LoadInt, .dst = 0, .immediate = scenario.left, .location = {}},
        {.opcode = Opcode::LoadInt, .dst = 1, .immediate = scenario.right, .location = {}},
        {.opcode = scenario.opcode, .dst = 2, .lhs = 0, .rhs = 1, .location = {}, .handler = 0},
        {.opcode = Opcode::Return, .dst = 2, .location = {}},
        {.opcode = Opcode::Catch, .dst = 3, .location = {}},
        {.opcode = Opcode::Return, .dst = 0, .location = {}},
    };
    Optimize(function, {});
    EXPECT_TRUE(std::ranges::any_of(function.instructions, [&](const auto& instruction) {
      return instruction.opcode == scenario.opcode && instruction.handler.has_value();
    }));
    EXPECT_FALSE(function.handlers.empty());
  }
}

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
  constexpr std::int64_t kResult = 42;
  constexpr std::uint32_t kFalseTarget = 6;
  Function function;
  function.register_count = 3;
  function.instructions = {
      {.opcode = Opcode::LoadInt, .dst = 0, .immediate = kResult, .location = {}},
      {.opcode = Opcode::Move, .dst = 1, .lhs = 0, .location = {}},
      {.opcode = Opcode::Move, .dst = 1, .lhs = 0, .location = {}},
      {.opcode = Opcode::LoadBool, .dst = 2, .immediate = 1, .location = {}},
      {.opcode = Opcode::JumpIfFalse, .lhs = 2, .index = kFalseTarget, .location = {}},
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

TEST(AnalysisTest, AnUnreachableFinalizerDoesNotLeaveExceptionTargetsPastTheCode) {
  constexpr std::int64_t kUnreachableResult = 42;
  Function function;
  function.register_count = 2;
  function.handlers = {{.target = 3}};
  function.instructions = {
      {.opcode = Opcode::LoadBool, .dst = 0, .immediate = 1, .location = {}, .handler = 0},
      {.opcode = Opcode::JumpIfFalse, .lhs = 0, .index = 3, .location = {}, .handler = 0},
      {.opcode = Opcode::Jump, .index = 0, .location = {}, .handler = 0},
      {.opcode = Opcode::LoadInt, .dst = 1, .immediate = kUnreachableResult, .location = {}},
      {.opcode = Opcode::Print, .dst = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
  };
  Optimize(function, {});
  EXPECT_TRUE(function.handlers.empty());
  EXPECT_FALSE(std::ranges::any_of(function.instructions, [](const auto& instruction) {
    return instruction.handler.has_value() || instruction.opcode == Opcode::Print;
  }));
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
  constexpr RegisterId kWordBits = std::numeric_limits<std::uint64_t>::digits;
  constexpr auto kPreviousWord = kWordBits - 1;
  constexpr auto kDistantWord = kWordBits * 4;
  constexpr auto kResult = kDistantWord + 1;
  constexpr auto kCaught = kWordBits + 1;
  constexpr std::uint32_t kNormalExit = 5;
  constexpr std::uint32_t kExceptionTarget = kNormalExit + 1;
  Function function;
  function.register_count = kResult + 1;
  function.handlers = {{.target = kExceptionTarget}};
  function.instructions = {
      {.opcode = Opcode::Jump, .index = 1, .location = {}},
      {.opcode = Opcode::JumpIfFalse, .lhs = kWordBits, .index = kNormalExit, .location = {}},
      {
          .opcode = Opcode::CheckedCast,
          .dst = kResult,
          .lhs = kPreviousWord,
          .location = {},
          .handler = 0,
      },
      {.opcode = Opcode::Move, .dst = kPreviousWord, .lhs = kDistantWord, .location = {}},
      {.opcode = Opcode::Jump, .index = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = kResult, .location = {}},
      {.opcode = Opcode::Catch, .dst = kCaught, .location = {}},
      {.opcode = Opcode::Return, .dst = kResult, .location = {}},
  };
  const auto graph = BuildFlowGraph(function);
  const auto live = AnalyzeLiveness(function, graph, {});
  std::vector<RegisterId> entry;
  live.front().live_in.ForEach([&](RegisterId reg) { entry.push_back(reg); });
  EXPECT_EQ(entry, (std::vector<RegisterId>{kPreviousWord, kWordBits, kDistantWord, kResult}));
  const auto& before_cast = live.at(graph.instruction_blocks.at(2)).live_in;
  EXPECT_TRUE(before_cast.Has(kPreviousWord));
  EXPECT_TRUE(before_cast.Has(kResult));
  EXPECT_FALSE(before_cast.Has(kCaught));
}

TEST(AnalysisTest, ExceptionalTransferKillsOnlyDefinitionsCompletedBeforeFailure) {
  constexpr std::uint32_t kRegisterCount = 5;
  constexpr std::int64_t kBeforeCast = 42;
  Function function;
  function.register_count = kRegisterCount;
  function.handlers = {{.target = 4}};
  function.instructions = {
      {.opcode = Opcode::LoadInt, .dst = 0, .immediate = kBeforeCast, .location = {}, .handler = 0},
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

TEST(AnalysisTest, RecoveryUsesKeepThePreviousDefinitionAliveAtAnOverwrite) {
  Function function;
  function.parameter_count = 1;
  function.register_count = 2;
  function.instructions = {
      {.opcode = Opcode::LoadString, .dst = 1, .location = {}},
      {.opcode = Opcode::Jump, .index = 2, .location = {}},
      {.opcode = Opcode::CheckedCast, .dst = 1, .lhs = 0, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
  };
  std::vector<std::vector<RegisterId>> required(function.instructions.size());
  required.at(2) = {1};
  const auto graph = BuildFlowGraph(function);
  const auto normal = AnalyzeLiveness(function, graph, {});
  const auto recovery = AnalyzeLiveness(function, graph, {}, required);
  const auto block = graph.instruction_blocks.at(2);
  EXPECT_FALSE(normal.at(block).live_in.Has(1));
  EXPECT_TRUE(recovery.at(block).live_in.Has(1));
  RegisterSet live(function.register_count);
  live.Insert(1);
  const auto& instruction = function.instructions.at(2);
  TransferLiveness(function, graph, recovery, instruction, AnalyzeRegisters(instruction, {}), live,
                   required.at(2));
  EXPECT_TRUE(live.Has(0));
  EXPECT_TRUE(live.Has(1));
}

}  // namespace
}  // namespace vela::ir
