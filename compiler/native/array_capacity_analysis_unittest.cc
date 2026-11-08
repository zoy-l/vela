#include "compiler/native/array_capacity_analysis.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::native {
namespace {

TEST(ArrayCapacityAnalysisTest, ProvesReservedZeroBasedSingleAppendLoops) {
  for (const auto* body : {
           "for (let index = 0; index < bound; index++) { values.push(index); }",
           "for (let index = 0; index < bound; index++) { if (index == 2) { continue; } "
           "values.push(index); }",
       }) {
    const auto prepared = ir::test::Prepare(
        "export func fill(bound: int): int { const values: int[] = []; values.reserve(bound); " +
        std::string(body) + " return values.length; }");
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto& functions = prepared.program.functions;
    const auto found = std::ranges::find(functions, "fill", &ir::Function::name);
    ASSERT_NE(found, functions.end());
    const auto accesses = ir::AnalyzeRegisterAccesses(*found, functions);
    const auto proved = FindReservedArrayPushes(*found, accesses);
    EXPECT_EQ(std::ranges::count(proved, 1), 1) << body;
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
    SCOPED_TRACE(body);
    const auto prepared = ir::test::Prepare(
        "external func publish(values: int[]): void; export func fill(bound: int): int { "
        "const values: int[] = []; " +
        std::string(body) + " return values.length; }");
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto& functions = prepared.program.functions;
    const auto found = std::ranges::find(functions, "fill", &ir::Function::name);
    ASSERT_NE(found, functions.end());
    const auto accesses = ir::AnalyzeRegisterAccesses(*found, functions);
    const auto proved = FindReservedArrayPushes(*found, accesses);
    EXPECT_EQ(std::ranges::count(proved, 1), 0);
  }
}

}  // namespace
}  // namespace vela::compiler::native
