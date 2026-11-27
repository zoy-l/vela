#include "compiler/native/array_capacity_analysis.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::native {
namespace {

void ExpectReservedPushes(std::string_view body, std::ptrdiff_t expected) {
  SCOPED_TRACE(body);
  const auto prepared = ir::test::Prepare(
      "external func publish(values: int[]): void; export func fill(bound: int): int { "
      "const values: int[] = []; " +
      std::string(body) + " return values.length; }");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& functions = prepared.program.functions;
  const auto fill = std::ranges::find(functions, "fill", &ir::Function::name);
  ASSERT_NE(fill, functions.end());
  const auto accesses = ir::AnalyzeRegisterAccesses(*fill, functions);
  EXPECT_EQ(std::ranges::count(FindReservedArrayPushes(*fill, accesses), 1), expected);
}

TEST(ArrayCapacityAnalysisTest, ProvesReservedZeroBasedSingleAppendLoops) {
  for (const auto* body : {
           "values.reserve(bound); for (let index = 0; index < bound; index++) { "
           "values.push(index); }",
           "values.reserve(bound); for (let index = 0; index < bound; index++) { "
           "if (index == 2) { continue; } values.push(index); }",
       }) {
    ExpectReservedPushes(body, 1);
  }
}

TEST(ArrayCapacityAnalysisTest, RetainsGrowthForUnprovedBoundsAliasesAndControlFlow) {
  for (const std::string_view body : {
           "values.push(42); values.reserve(bound); for (let i = 0; i < bound; i++) { "
           "values.push(i); }",
           "values.reserve(1); for (let i = 0; i < bound; i++) { values.push(i); }",
           "values.reserve(bound); for (let i = 0; i < bound; i++) { values.push(i); "
           "values.push(i); }",
           "values.reserve(bound); for (let i = 0; i < bound; i++) { bound++; values.push(i); }",
           "values.reserve(bound); for (let i = 0; i < bound; i++) { values.clear(); "
           "values.push(i); }",
           "values.reserve(bound); for (let i = 0; i < bound; i++) { for (let j = 0; j < bound; "
           "j++) { values.push(j); } }",
           "let alias = values; alias = []; publish(values); values.reserve(bound); for (let i = "
           "0; i < bound; i++) { values.push(i); }",
       }) {
    ExpectReservedPushes(body, 0);
  }
}

}  // namespace
}  // namespace vela::compiler::native
