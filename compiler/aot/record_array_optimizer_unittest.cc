#include "compiler/aot/record_array_optimizer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "base/opcode.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::aot {
namespace {

using ir::test::Prepare;

constexpr std::string_view kRecordSource = R"(
class Point { x: double; count: int; }
let points: Point[] = [];
export func prepare(): int {
  points.reserve(8);
  return points.push(Point(x: 1.5, count: 3));
}
)";

std::size_t RecordLayouts(const ir::Program& program) {
  return static_cast<std::size_t>(std::ranges::count_if(
      program.layouts.arrays, [](const auto& array) { return array.record_fields != 0; }));
}

TEST(RecordArrayTest, PacksModuleLocalArraysAndPreservesCheckedLogicalIndices) {
  const auto prepared = Prepare(std::string(kRecordSource) + R"(
export func update(index: int): double {
  const point = points[index];
  point.count += 1;
  point.x += point.count.toDouble();
  return point.x;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto artifact =
      Compile(prepared.program, {.exports = {"prepare", "update"}, .preserve_ir = true});
  ASSERT_TRUE(artifact) << frontend::DiagnosticsText(artifact.error());
  EXPECT_EQ(RecordLayouts(artifact->program), 1U);
  const auto found = std::ranges::find(artifact->program.functions, "update", &ir::Function::name);
  ASSERT_NE(found, artifact->program.functions.end());
  EXPECT_EQ(
      std::ranges::count(found->instructions, Opcode::CheckRecordIndex, &ir::Instruction::opcode),
      1);
  EXPECT_EQ(
      std::ranges::count(found->instructions, Opcode::SetRecordField, &ir::Instruction::opcode), 2);
  EXPECT_EQ(std::ranges::count(found->instructions, Opcode::GetField, &ir::Instruction::opcode), 0);
  EXPECT_NE(artifact->llvm_ir.find("icmp ult i64"), std::string::npos);
}

TEST(RecordArrayTest, RejectsIdentityEscapeAndBorrowsAcrossCallsOrControlBoundaries) {
  constexpr std::array kReaders{
      "export func read(): Point { return points[0]; }",
      "export func read(): Point[] { return points; }",
      "export func read(): bool { return points[0] == points[1]; }",
      "external func observe(): void; export func read(): double { "
      "const point = points[0]; observe(); return point.x; }",
      "export func read(): double { const point = points[0]; "
      "for (let i = 0; i < 8; i++) { point.x += 1.0; } return point.x; }",
      "export func read(): double { const point = points[0]; "
      "points.reserve(32); return point.x; }",
      "export func read(): double { const point = points[0]; "
      "points.clear(); return point.x; }",
  };
  for (const auto* reader : kReaders) {
    SCOPED_TRACE(reader);
    auto prepared = Prepare(std::string(kRecordSource) + reader);
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    PackPrivateRecordArrays(prepared.program);
    EXPECT_EQ(RecordLayouts(prepared.program), 0U);
  }
}

TEST(RecordArrayTest, RejectsSharedInsertionsAndPostInsertionAliases) {
  constexpr std::array kWriters{
      "const point = Point(x: 1.0, count: 1); points.push(point); return points.push(point);",
      "const point = Point(x: 1.0, count: 1); points.push(point); point.x += 1.0; return 0;",
      "const point = Point(x: 1.0, count: 1); points.push(point); return point.count;",
  };
  for (const auto* writer : kWriters) {
    SCOPED_TRACE(writer);
    auto prepared = Prepare(
        "class Point { x: double; count: int; } let points: Point[] = []; "
        "export func write(): int { " +
        std::string(writer) + " }");
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    PackPrivateRecordArrays(prepared.program);
    EXPECT_EQ(RecordLayouts(prepared.program), 0U);
  }
}

TEST(RecordArrayTest, KeepsExportedGlobalsAndReferenceFieldsInOrdinaryStorage) {
  constexpr std::array kSources{
      "class Point { x: double; } export let points: Point[] = []; "
      "export func append(): int { return points.push(Point(x: 1.0)); }",
      "class Point { x: string; } let points: Point[] = []; "
      "export func append(): int { return points.push(Point(x: 'value')); }",
      "class Point { x: double?; } let points: Point[] = []; "
      "export func append(): int { return points.push(Point(x: null)); }",
      "class Point { x: double; } [Reflect] class Store { "
      " static #points: Point[] = [];   "
      "static append(): int { return Store.#points.push(Point(x: 1.0)); } } "
      "export func append(): int { return Store.append(); }",
  };
  for (const auto* source : kSources) {
    SCOPED_TRACE(source);
    auto prepared = Prepare(source);
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    PackPrivateRecordArrays(prepared.program);
    EXPECT_EQ(RecordLayouts(prepared.program), 0U);
  }
}

}  // namespace
}  // namespace vela::compiler::aot
