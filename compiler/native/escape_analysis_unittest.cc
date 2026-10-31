#include "compiler/native/escape_analysis.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "base/opcode.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation_test_support.h"
#include "compiler/native/borrow_analysis.h"
#include "compiler/native/native_roots.h"

namespace vela::compiler::native {
namespace {

using ir::test::Prepare;

std::size_t FunctionIndex(const ir::Program& program, std::string_view name) {
  const auto found = std::ranges::find(program.functions, name, &ir::Function::name);
  return static_cast<std::size_t>(found - program.functions.begin());
}

std::expected<EscapePlan, std::string> Analyze(std::string_view source) {
  const auto program = Prepare(source);
  if (!program.Succeeded()) {
    return std::unexpected(program.diagnostics.front().message);
  }
  return PlanEscapes(program.program, FunctionIndex(program.program, "sum"));
}

TEST(EscapeAnalysisTest, EliminatesLocalAliasesAndMergedFieldStates) {
  const auto program = Prepare(R"(
class Point { x: int; y: int; }
export func sum(x: int, choose: bool): int {
  const point = Point(x: x, y: 2);
  const alias = point;
  if (choose) { alias.x = 40; } else { point.x = 10; }
  for (let index = 0; index < 3; index++) { point.y += index; }
  return alias.x + point.y;
}
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto plan = PlanEscapes(program.program, FunctionIndex(program.program, "sum"));
  ASSERT_EQ(plan.allocations.size(), 1U);
  EXPECT_EQ(plan.allocations.front().decision, AllocationDecision::ScalarReplacement);
  for (const auto& instruction : plan.function.instructions) {
    EXPECT_NE(instruction.opcode, Opcode::NewInstance);
    EXPECT_NE(instruction.opcode, Opcode::GetField);
    EXPECT_NE(instruction.opcode, Opcode::SetField);
  }
  const auto compiled = aot::Compile(program.program, {.exports = {"sum"}, .preserve_ir = true});
  ASSERT_TRUE(compiled) << compiled.error().front().message;
  EXPECT_EQ(compiled->llvm_ir.find("@vela_native_v1_new_instance"), std::string_view::npos);
  EXPECT_EQ(compiled->function_metrics.front().eliminated_allocations, 1U);
}

TEST(EscapeAnalysisTest, RepeatedAllocationDoesNotConflateLiveIterationInstances) {
  const auto plan = Analyze(R"(
class Point { x: int; }
export func sum(count: int): int {
  let previous = Point(x: 1);
  let result = 0;
  for (let index = 0; index < count; index++) {
    const current = Point(x: index);
    result += previous.x;
    previous = current;
  }
  return result + previous.x;
}
)");
  ASSERT_TRUE(plan) << plan.error();
  ASSERT_EQ(plan->allocations.size(), 2U);
  EXPECT_NE(plan->allocations.back().decision, AllocationDecision::ScalarReplacement);
}

TEST(EscapeAnalysisTest, NonOverlappingLoopInstancesCanUseTheSameFieldSlots) {
  const auto plan = Analyze(R"(
class Point { x: int; }
export func sum(count: int): int {
  let result = 0;
  for (let index = 0; index < count; index++) {
    const point = Point(x: index);
    result += point.x;
  }
  return result;
}
)");
  ASSERT_TRUE(plan) << plan.error();
  ASSERT_EQ(plan->allocations.size(), 1U);
  EXPECT_EQ(plan->allocations.front().decision, AllocationDecision::ScalarReplacement);
}

TEST(EscapeAnalysisTest, SeparatelyDefinedRegisterValuesDoNotKeepAnOldAlias) {
  const auto plan = Analyze(R"(
class Point { x: int; }
export func sum(): int {
  let point = Point(x: 20);
  const first = point.x;
  point = Point(x: 22);
  return first + point.x;
}
)");
  ASSERT_TRUE(plan) << plan.error();
  ASSERT_EQ(plan->allocations.size(), 2U);
  for (const auto& allocation : plan->allocations) {
    EXPECT_EQ(allocation.decision, AllocationDecision::ScalarReplacement);
  }
}

TEST(EscapeAnalysisTest, ReferenceFieldsBecomePreciseRootsAcrossUnrelatedCalls) {
  const auto program = Prepare(R"(
external func collect(): void;
class Box { text: string; }
export func size(text: string): int {
  const box = Box(text: text + 'suffix');
  collect();
  return box.text.length;
}
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto plan = PlanEscapes(program.program, FunctionIndex(program.program, "size"));
  ASSERT_EQ(plan.allocations.size(), 1U);
  const auto& allocation = plan.allocations.front();
  ASSERT_EQ(allocation.decision, AllocationDecision::ScalarReplacement);
  const auto roots = PlanNativeRoots(plan.function, program.program.functions,
                                     program.program.native_mode, NativeDebugging::Safepoints);
  ASSERT_EQ(allocation.fields.size(), 1U);
  const auto field = allocation.fields.front();
  EXPECT_TRUE(roots.slots.at(field));
  bool rooted = false;
  for (std::size_t index = 0; index < plan.function.instructions.size(); ++index) {
    if (plan.function.instructions.at(index).opcode == Opcode::Call) {
      rooted = std::ranges::find(roots.stores.at(index), field) != roots.stores.at(index).end();
    }
  }
  EXPECT_TRUE(rooted);
}

TEST(EscapeAnalysisTest, KeepsHeapObjectsAtUnprovenConstructionAndPublicationBoundaries) {
  const auto program = Prepare(R"(
class Point { x: int; }
class Custom { x: int; constructor(value: int) { this.x = value; } }
export func create(): Point { return Point(x: 42); }
export func read(): int { return Custom(value: 42).x; }
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto published = PlanEscapes(program.program, FunctionIndex(program.program, "create"));
  const auto constructed = PlanEscapes(program.program, FunctionIndex(program.program, "read"));
  ASSERT_EQ(published.allocations.size(), 1U);
  ASSERT_EQ(constructed.allocations.size(), 1U);
  EXPECT_EQ(published.allocations.front().decision, AllocationDecision::ScalarReplacement);
  EXPECT_TRUE(published.allocations.front().materialized);
  EXPECT_EQ(constructed.allocations.front().decision, AllocationDecision::CallBoundary);
}

TEST(EscapeAnalysisTest, PreservesIdentityWithoutAllocatingEqualFieldValues) {
  const auto program = Prepare(R"(
class Point { x: int; }
export func same(): bool {
  const first = Point(x: 42);
  const second = Point(x: 42);
  const alias = first;
  return first == alias && first != second;
}
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto compiled = aot::Compile(program.program, {.exports = {"same"}, .preserve_ir = true});
  ASSERT_TRUE(compiled) << compiled.error().front().message;
  EXPECT_EQ(compiled->function_metrics.front().eliminated_allocations, 2U);
  EXPECT_EQ(compiled->llvm_ir.find("@vela_native_v1_new_instance"), std::string_view::npos);
}

TEST(EscapeAnalysisTest, SinksPublicationAndReconcilesHeapAndVirtualBranches) {
  const auto program = Prepare(R"(
class Point { x: int; }
external func publish(point: Point): void;
export func read(publishNow: bool): int {
  const point = Point(x: 42);
  const alias = point;
  if (publishNow) { publish(alias); }
  return point.x;
}
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto plan = PlanEscapes(program.program, FunctionIndex(program.program, "read"));
  ASSERT_EQ(plan.allocations.size(), 1U);
  EXPECT_TRUE(plan.allocations.front().materialized);
  const auto compiled = aot::Compile(program.program, {.exports = {"read"}, .preserve_ir = true});
  ASSERT_TRUE(compiled) << compiled.error().front().message;
  for (const auto& metrics : compiled->function_metrics) {
    EXPECT_EQ(metrics.retained_allocations, 0U);
  }
}

TEST(EscapeAnalysisTest, PlansOneObjectPerNodeForSharedCyclicGraphs) {
  const auto program = Prepare(R"(
class Link { value: int; next: Link?; other: Link?; }
export func create(): Link {
  const first = Link(value: 20, next: null, other: null);
  const second = Link(value: 22, next: first, other: first);
  first.next = second;
  first.other = second;
  return first;
}
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto plan = PlanEscapes(program.program, FunctionIndex(program.program, "create"));
  ASSERT_EQ(plan.allocations.size(), 2U);
  for (const auto& allocation : plan.allocations) {
    EXPECT_EQ(allocation.decision, AllocationDecision::ScalarReplacement);
    EXPECT_TRUE(allocation.materialized);
  }
  const auto compiled = aot::Compile(program.program, {.exports = {"create"}, .preserve_ir = true});
  ASSERT_TRUE(compiled) << compiled.error().front().message;
}

TEST(EscapeAnalysisTest, FoldsNominalTestsAndProvenCastsWithoutLosingAliases) {
  const auto program = Prepare(R"(
interface Sized { size(): int; }
class Box implements Sized { value: int; size(): int { return this.value; } }
class Other { value: int; }
export func read(): int {
  const box = Box(value: 42);
  const erased: dynamic = box;
  const alias = erased as Box;
  return erased is Box && erased is Sized && !(erased is Other) ? alias.value : 0;
}
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto compiled = aot::Compile(program.program, {.exports = {"read"}, .preserve_ir = true});
  ASSERT_TRUE(compiled) << compiled.error().front().message;
  EXPECT_EQ(compiled->llvm_ir.find("@vela_native_v1_new_instance"), std::string_view::npos);
  EXPECT_EQ(compiled->llvm_ir.find("@vela_native_v1_type_test"), std::string_view::npos);
}

TEST(EscapeAnalysisTest, ValidationConfigurationCompilesTheSameSourceWithHeapStorage) {
  const auto program = Prepare(R"(
class Point { x: int; }
export func read(): int { return Point(x: 42).x; }
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto compiled = aot::Compile(
      program.program,
      {.exports = {"read"}, .preserve_ir = true, .escape = EscapeOptimization::Disabled});
  ASSERT_TRUE(compiled) << compiled.error().front().message;
  EXPECT_NE(compiled->llvm_ir.find("@vela_native_v1_new_instance"), std::string_view::npos);
  EXPECT_EQ(compiled->function_metrics.front().eliminated_allocations, 0U);
  EXPECT_EQ(compiled->function_metrics.front().retained_allocations, 1U);
}

TEST(EscapeAnalysisTest, PrivateBorrowEntriesPreserveWritesWithoutHeapAllocation) {
  const auto program = Prepare(R"(
class Box { value: int; text: string; }
func update(box: Box, value: int): int {
  const alias = box;
  alias.value += value;
  return box.value;
}
export func read(): int {
  const box = Box(value: 20, text: 'root');
  update(box, 22);
  return box.value;
}
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto borrows = PlanBorrows(program.program);
  ASSERT_TRUE(borrows.at(FunctionIndex(program.program, "update")));
  const auto compiled = aot::Compile(program.program, {.exports = {"read"}, .preserve_ir = true});
  ASSERT_TRUE(compiled) << compiled.error().front().message;
  const auto& metrics = compiled->function_metrics.at(FunctionIndex(compiled->program, "read"));
  EXPECT_EQ(metrics.stack_records, 1U);
  EXPECT_EQ(metrics.eliminated_allocations, 1U);
  const auto& borrowed = compiled->function_metrics.at(FunctionIndex(compiled->program, "update"));
  EXPECT_GT(borrowed.borrowed_frame_bytes, 0U);
  EXPECT_EQ(borrowed.borrowed_shadow_slots, 1U);
  const auto scalar = aot::Compile(
      program.program, {.exports = {"read"}, .escape = EscapeOptimization::ScalarReplacement});
  ASSERT_TRUE(scalar) << scalar.error().front().message;
  EXPECT_EQ(scalar->function_metrics.at(FunctionIndex(scalar->program, "read")).stack_records, 0U);
  EXPECT_EQ(scalar->function_metrics.at(FunctionIndex(scalar->program, "read")).sunk_allocations,
            1U);
}

TEST(EscapeAnalysisTest, BorrowProofRejectsIdentityPublicationCaptureAndAsync) {
  const auto program = Prepare(R"(
class Box { value: int; next: Box?; static saved: Box? = null; }
external func host(box: Box): void;
func save(box: Box): void { Box.saved = box; }
func self(box: Box): void { box.next = box; }
func forward(box: Box): void { host(box); }
func capture(box: Box): () => int { return (): int => box.value; }
async func suspend(box: Box): Future<int> { await sleep(1); return box.value; }
)");
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto borrows = PlanBorrows(program.program);
  for (const auto* const name : {"save", "self", "forward", "capture", "suspend"}) {
    EXPECT_FALSE(borrows.at(FunctionIndex(program.program, name))) << name;
  }
}

TEST(EscapeAnalysisTest, OverBudgetRecordsSelectHeapCallsBeforeBodyEmission) {
  std::string source = "class Wide { ";
  constexpr std::size_t kFields = 300;
  for (std::size_t field = 0; field < kFields; ++field) {
    source += "value" + std::to_string(field) + ": int; ";
  }
  source +=
      "} func read(box: Wide): int { return box.value0; } "
      "export func run(): int { const box = Wide(";
  for (std::size_t field = 0; field < kFields; ++field) {
    source += (field == 0 ? "" : ", ") + std::string("value") + std::to_string(field) + ": 0";
  }
  source += "); return read(box); }";
  const auto program = Prepare(source);
  ASSERT_TRUE(program.Succeeded()) << program.diagnostics.front().message;
  const auto compiled = aot::Compile(program.program, {.exports = {"run"}});
  ASSERT_TRUE(compiled) << compiled.error().front().message;
  const auto& metrics = compiled->function_metrics.at(FunctionIndex(compiled->program, "run"));
  EXPECT_EQ(metrics.stack_records, 0U);
  EXPECT_EQ(metrics.sunk_allocations, 1U);
}

}  // namespace
}  // namespace vela::compiler::native
