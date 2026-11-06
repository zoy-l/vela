#include "compiler/native/field_access.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_roots.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::native {
namespace {

TEST(FieldAccessTest, KeepsInitializationAndKindTransitionsWhileRemovingRepeatedScalarKinds) {
  const auto prepared = ir::test::Prepare(R"(
class Point { x: double; y: double; optional: double?; any: dynamic; }
export func make(): Point { return Point(x: 1.0, y: 2.0, optional: null, any: null); }
export func update(point: Point): void { point.x += 1.0; point.y += 2.0; }
export func transitions(point: Point): void {
  point.optional = 1.0; point.optional = 2.0; point.optional = null;
  point.any = 3.0; point.any = 4.0; point.any = 'changed'; point.any = 5.0;
}
export func aliased(point: Point, alias: Point): void {
  const previous = point.x; alias.x = 0.0; point.x = previous + 1.0;
  point.any = 1.0; alias.any = 'changed'; point.any = 2.0;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  for (const auto& function : program.functions) {
    const auto roots = PlanNativeRoots(function, program.functions, program.native_mode,
                                       NativeDebugging::Safepoints);
    const auto writes = FindFieldKindWrites(function, program, roots.safepoints);
    const auto expected = function.name == "update" ? 2 : 0;
    EXPECT_EQ(std::ranges::count(writes, FieldKindWrite::Redundant), expected) << function.name;
  }
}

TEST(FieldAccessTest, DropsKnowledgeAcrossCallsControlFlowAndReceiverReassignments) {
  const auto prepared = ir::test::Prepare(R"(
class Point { x: double; }
external func change(point: Point): void;
export func called(point: Point): void {
  const previous = point.x; change(point); point.x = previous + 1.0;
}
export func branched(point: Point, choose: bool): void {
  const previous = point.x; if (choose) { print(0); } point.x = previous + 1.0;
}
export func reassigned(first: Point, second: Point): void {
  let point = first; const previous = point.x; point = second; point.x = previous + 1.0;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  for (const auto& function : program.functions) {
    const auto roots = PlanNativeRoots(function, program.functions, program.native_mode,
                                       NativeDebugging::Safepoints);
    const auto writes = FindFieldKindWrites(function, program, roots.safepoints);
    EXPECT_EQ(std::ranges::count(writes, FieldKindWrite::Redundant), 0) << function.name;
  }
}

TEST(FieldAccessTest, DoesNotCarryTagKnowledgeIntoDebugSafepointsOrReloadableRecovery) {
  auto prepared = ir::test::Prepare(R"(
class Point { x: double; }
export func update(point: Point): void { point.x += 1.0; }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  auto& program = prepared.program;
  const auto found = std::ranges::find(program.functions, "update", &ir::Function::name);
  ASSERT_NE(found, program.functions.end());
  const auto& function = *found;
  const auto roots = PlanNativeRoots(function, program.functions, program.native_mode,
                                     NativeDebugging::Instructions);
  const auto writes = FindFieldKindWrites(function, program, roots.safepoints);
  EXPECT_EQ(std::ranges::count(writes, FieldKindWrite::Redundant), 0);
  program.native_mode = NativeProgramMode::Reloadable;
  const std::vector<std::uint8_t> safepoints(function.instructions.size());
  const auto recovery = FindFieldKindWrites(function, program, safepoints);
  EXPECT_EQ(std::ranges::count(recovery, FieldKindWrite::Redundant), 0);
}

}  // namespace
}  // namespace vela::compiler::native
