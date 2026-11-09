#include "compiler/native/native_roots.h"

#include <algorithm>
#include <array>
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
#include "compiler/native/code_generator.h"
#include "compiler/native/global_analysis.h"
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

TEST(NativeRootRecoveryTest, ReservesNullInitializedReferencesAndCopiesAfterMaterialization) {
  ir::Function function;
  function.return_type = {
      .kind = frontend::TypeDescriptor::Kind::Class,
      .name = "Box",
      .nullable = true,
  };
  function.register_count = 2;
  function.value_types = {function.return_type, function.return_type};
  function.instructions = {
      {.opcode = Opcode::LoadNull, .dst = 0, .location = {}},
      {.opcode = Opcode::Move, .dst = 1, .lhs = 0, .location = {}},
      {.opcode = Opcode::Print, .lhs = 1, .location = {.line = 2}},
      {.opcode = Opcode::Return, .dst = 0, .location = {}},
  };
  const auto ordinary =
      PlanNativeRoots(function, {}, NativeProgramMode::Reloadable, NativeDebugging::SourceLines);
  EXPECT_EQ(ordinary.count, 0U);
  const std::array<ir::RegisterId, 1> materialized{0};
  const auto recovery =
      PlanNativeRoots(function, {}, NativeProgramMode::Reloadable, NativeDebugging::SourceLines,
                      {.values = {}, .safepoints = {}, .materialized = materialized});
  EXPECT_TRUE(recovery.slots.front());
  EXPECT_TRUE(recovery.slots.back());
  EXPECT_EQ(recovery.count, 2U);
  ExpectDistinctLiveRoots(function, {}, recovery);
}

TEST(NativeRootRecoveryTest, ReservesRecoveryCapacityAndMetadataOnlyReferences) {
  using Kind = frontend::TypeDescriptor::Kind;
  const frontend::TypeDescriptor text{.kind = Kind::String, .name = frontend::kStringTypeName};
  ir::Function function;
  function.return_type = text;
  function.parameter_count = 2;
  function.register_count = 2;
  function.value_types = {text, text};
  function.instructions = {
      {.opcode = Opcode::Print, .lhs = 0, .location = {}},
      {.opcode = Opcode::Print, .lhs = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
  };
  std::vector<std::vector<ir::RegisterId>> required(function.instructions.size());
  required.back() = {0};
  constexpr std::size_t kRecoveryRoots = 8;
  const auto plan =
      PlanNativeRoots(function, {}, NativeProgramMode::Reloadable, NativeDebugging::Safepoints,
                      {
                          .values = required,
                          .safepoints = {},
                          .materialized = {},
                          .minimum_root_count = kRecoveryRoots,
                      });
  EXPECT_EQ(plan.count, kRecoveryRoots);
  ASSERT_TRUE(plan.slots.front());
  ASSERT_TRUE(plan.slots.back());
  EXPECT_NE(*plan.slots.front(), *plan.slots.back());
  ExpectDistinctLiveRoots(function, {}, plan, required);
}

TEST(NativeInstanceStringRootsTest, OmitsOnlyStableInstanceStringsAndKeepsOverwrittenRegisters) {
  const auto prepared = ir::test::Prepare(R"(
external func consume(text: string): void;
export func constant(): void {
  const text = 'constant'; const copy = text;
  for (let index = 0; index < 8; index++) { consume(copy); }
}
export func changed(flag: bool, input: string): void {
  let text = 'constant'; if (flag) { text = input; }
  consume(text);
}
export func merged(flag: bool): void {
  let text = 'first'; if (flag) { text = 'second'; }
  consume(text);
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto& functions = prepared.program.functions;
  const auto stable = std::ranges::find(functions, "constant", &ir::Function::name);
  const auto changed = std::ranges::find(functions, "changed", &ir::Function::name);
  const auto merged = std::ranges::find(functions, "merged", &ir::Function::name);
  ASSERT_NE(stable, functions.end());
  ASSERT_NE(changed, functions.end());
  ASSERT_NE(merged, functions.end());
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    const auto plan = PlanNativeRoots(*stable, functions, mode, NativeDebugging::Safepoints);
    EXPECT_EQ(plan.count, 0U);
    EXPECT_EQ(PlanNativeRoots(*merged, functions, mode, NativeDebugging::Safepoints).count, 0U);
    const auto reassigned = PlanNativeRoots(*changed, functions, mode, NativeDebugging::Safepoints);
    EXPECT_GT(reassigned.count, 0U);
    ExpectDistinctLiveRoots(*changed, functions, reassigned);
  }
}

TEST(NativeInstanceRootsTest, CoversReadonlyGlobalCopiesAndNullAcrossLoopsOnlyInClosedAot) {
  const auto prepared = ir::test::Prepare(R"(
class Box { value: int = 1; }
let first = Box();
let second = Box();
external func consume(value: Box?): void;
export func run(count: int): void {
  let argument: Box? = null;
  for (let index = 0; index < count; index++) {
    const previous = argument;
    if (index % 4 == 0) { argument = first; }
    else if (index % 4 == 1) { argument = second; }
    else if (index % 4 == 2) { argument = previous; }
    else { argument = null; }
    consume(argument);
  }
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto& functions = program.functions;
  const auto found = std::ranges::find(functions, "run", &ir::Function::name);
  ASSERT_NE(found, functions.end());
  const auto globals = AnalyzeGlobalAccess(program, NativeCodePlacement::Linked);
  ASSERT_FALSE(globals.readonly_slots.empty());
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    for (const auto debugging : {
             NativeDebugging::Safepoints,
             NativeDebugging::SourceLines,
             NativeDebugging::Instructions,
         }) {
      const auto plan =
          PlanNativeRoots(*found, functions, mode, debugging, {}, globals.readonly_slots);
      if (mode == NativeProgramMode::Closed && debugging == NativeDebugging::Safepoints) {
        EXPECT_EQ(plan.count, 0U);
      } else {
        EXPECT_GT(plan.count, 0U);
      }
      ExpectDistinctLiveRoots(*found, functions, plan);
    }
  }
}

TEST(NativeInstanceRootsTest, KeepsUnknownCopiesMutableFieldsAndSuspendedValuesRooted) {
  const auto prepared = ir::test::Prepare(R"(
class Box { child: Box? = null; }
let held = Box();
external func consume(value: Box?): void;
export func mixed(input: Box, flag: bool, count: int): void {
  let argument: Box? = held;
  for (let index = 0; index < count; index++) {
    const previous = argument;
    if (flag) { argument = input; } else { argument = previous; }
    consume(argument);
  }
}
export func field(): void { const child = held.child; consume(child); }
export async func suspended(): Future<void> {
  const saved = held;
  await sleep(0);
  consume(saved);
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto& functions = program.functions;
  const auto globals = AnalyzeGlobalAccess(program, NativeCodePlacement::Linked);
  for (const auto* name : {"mixed", "field", "suspended"}) {
    const auto found = std::ranges::find(functions, name, &ir::Function::name);
    ASSERT_NE(found, functions.end());
    const auto& function = *found;
    const auto plan = PlanNativeRoots(function, functions, NativeProgramMode::Closed,
                                      NativeDebugging::Safepoints, {}, globals.readonly_slots);
    EXPECT_GT(plan.count, 0U) << name;
    const auto consume = std::ranges::find_if(function.instructions, [&](const auto& instruction) {
      return instruction.opcode == Opcode::Call && functions[instruction.index].name == "consume";
    });
    ASSERT_NE(consume, function.instructions.end()) << name;
    EXPECT_TRUE(plan.slots.at(consume->lhs).has_value()) << name;
    ExpectDistinctLiveRoots(function, functions, plan);
  }
}

TEST(NativeInstanceRootsTest, KeepsValuesFromGloballyWritableSlotsRooted) {
  const auto prepared = ir::test::Prepare(R"(
class Box {}
let held = Box();
external func consume(value: Box): void;
export func write(input: Box): void { held = input; }
export func read(): void { const saved = held; consume(saved); }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto& functions = program.functions;
  const auto found = std::ranges::find(functions, "read", &ir::Function::name);
  ASSERT_NE(found, functions.end());
  const auto globals = AnalyzeGlobalAccess(program, NativeCodePlacement::Linked);
  const auto plan = PlanNativeRoots(*found, functions, NativeProgramMode::Closed,
                                    NativeDebugging::Safepoints, {}, globals.readonly_slots);
  EXPECT_GT(plan.count, 0U);
  ExpectDistinctLiveRoots(*found, functions, plan);
}

INSTANTIATE_TEST_SUITE_P(Debugging,
                         NativeRootsTest,
                         testing::Values(NativeDebugging::Safepoints,
                                         NativeDebugging::SourceLines,
                                         NativeDebugging::Instructions));

}  // namespace
}  // namespace vela::compiler::native
