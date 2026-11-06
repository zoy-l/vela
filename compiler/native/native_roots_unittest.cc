#include "compiler/native/native_roots.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/register_set.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::native {
namespace {

void ExpectDistinctLiveRoots(const ir::Function& function,
                             std::span<const ir::Function> functions,
                             const NativeRootPlan& plan,
                             std::span<const std::vector<ir::RegisterId>> required = {}) {
  const auto graph = ir::BuildFlowGraph(function);
  const auto blocks = ir::AnalyzeLiveness(function, graph, functions, required);
  for (std::size_t block_index = 0; block_index < graph.blocks.size(); ++block_index) {
    const auto& block = graph.blocks.at(block_index);
    auto live = blocks.at(block_index).live_out;
    for (auto position = block.end; position > block.begin;) {
      const auto index = --position;
      SCOPED_TRACE(index);
      const auto& instruction = function.instructions.at(index);
      const auto access = ir::AnalyzeRegisters(instruction, functions);
      auto simultaneous = live;
      ir::TransferLiveness(function, graph, blocks, instruction, access, live,
                           required.empty() ? std::span<const ir::RegisterId>{} : required[index]);
      simultaneous.UnionWith(live);
      for (const auto reg : plan.clears.at(index)) {
        simultaneous.Insert(reg);
      }
      std::set<std::size_t> occupied;
      simultaneous.ForEach([&](ir::RegisterId reg) {
        if (const auto slot = plan.slots.at(reg)) {
          EXPECT_LT(*slot, plan.count);
          EXPECT_TRUE(occupied.insert(*slot).second) << "register " << reg;
        }
      });
    }
  }
}

class NativeRootsTest : public testing::TestWithParam<NativeDebugging> {};

TEST_P(NativeRootsTest, ReusesSlotsBetweenSequentialReferenceLifetimes) {
  const auto prepared = ir::test::Prepare(R"(
external func retain(value: string): void;
func run(): void {
  const first = 'alpha'.toUpperCase(); retain(first);
  const second = 'beta'.toLowerCase(); retain(second);
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& functions = prepared.program.functions;
  const auto found = std::ranges::find(functions, "run", &ir::Function::name);
  ASSERT_NE(found, functions.end());
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    const auto plan = PlanNativeRoots(*found, functions, mode, GetParam());
    const auto assigned =
        std::ranges::count_if(plan.slots, [](const auto& slot) { return slot.has_value(); });
    EXPECT_LT(plan.count, static_cast<std::size_t>(assigned));
    ExpectDistinctLiveRoots(*found, functions, plan);
  }
}

TEST_P(NativeRootsTest, KeepsLoopValuesAndExceptionalAssignmentInputsDistinct) {
  const auto prepared = ir::test::Prepare(R"(
external func next(): string;
func run(repeat: bool): string {
  let held = next();
  while (repeat) {
    const tail = next();
    try { held = next(); } catch (error: Error) { return held + tail; }
    if (tail.length == 0) break;
  }
  return held;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& functions = prepared.program.functions;
  const auto found = std::ranges::find(functions, "run", &ir::Function::name);
  ASSERT_NE(found, functions.end());
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    const auto plan = PlanNativeRoots(*found, functions, mode, GetParam());
    ExpectDistinctLiveRoots(*found, functions, plan);
  }
}

TEST_P(NativeRootsTest, KeepsSuspendedReferencesDistinctBeforeOutcomeCollection) {
  const auto prepared = ir::test::Prepare(R"(
external func next(): string;
async func run(): Future<string> {
  const first = next(); await sleep(0);
  const second = next(); await sleep(0);
  return first + second;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& functions = prepared.program.functions;
  const auto found = std::ranges::find(functions, "run", &ir::Function::name);
  ASSERT_NE(found, functions.end());
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    const auto plan = PlanNativeRoots(*found, functions, mode, GetParam());
    ExpectDistinctLiveRoots(*found, functions, plan);
    std::size_t suspensions = 0;
    for (const auto& saved : plan.suspended) {
      if (saved.empty()) {
        continue;
      }
      ++suspensions;
      std::set<std::size_t> occupied;
      for (const auto reg : saved) {
        if (const auto slot = plan.slots.at(reg)) {
          EXPECT_TRUE(occupied.insert(*slot).second);
        }
      }
    }
    EXPECT_GT(suspensions, 0U);
  }
}

TEST(NativeRootRecoveryTest, ReservesRecoveryCapacityAndMetadataOnlyReferences) {
  using Kind = frontend::TypeDescriptor::Kind;
  const frontend::TypeDescriptor text{.kind = Kind::String, .name = frontend::kStringTypeName};
  ir::Function function;
  function.return_type = text;
  function.register_count = 2;
  function.value_types = {text, text};
  function.instructions = {
      {.opcode = Opcode::LoadString, .dst = 0, .location = {}},
      {.opcode = Opcode::Print, .lhs = 0, .location = {}},
      {.opcode = Opcode::LoadString, .dst = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
  };
  std::vector<std::vector<ir::RegisterId>> required(function.instructions.size());
  required.back() = {0};
  constexpr std::size_t kRecoveryRoots = 8;
  const auto plan =
      PlanNativeRoots(function, {}, NativeProgramMode::Reloadable, NativeDebugging::Safepoints,
                      {.values = required, .safepoints = {}, .minimum_root_count = kRecoveryRoots});
  EXPECT_EQ(plan.count, kRecoveryRoots);
  ASSERT_TRUE(plan.slots.front());
  ASSERT_TRUE(plan.slots.back());
  EXPECT_NE(*plan.slots.front(), *plan.slots.back());
  ExpectDistinctLiveRoots(function, {}, plan, required);
}

INSTANTIATE_TEST_SUITE_P(Debugging,
                         NativeRootsTest,
                         testing::Values(NativeDebugging::Safepoints,
                                         NativeDebugging::SourceLines,
                                         NativeDebugging::Instructions));

}  // namespace
}  // namespace vela::compiler::native
