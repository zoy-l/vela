#include "compiler/native/map_analysis.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/ir/ir.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::native {
namespace {

TEST(MapAnalysisTest, ReusesLookupsAcrossNullableCoalescingAndScalarBranches) {
  const auto prepared = ir::test::Prepare(R"(
export func update(map: Map<string, int>, key: string, choose: bool): void {
  const alias = map;
  const value = alias.get(key) ?? 0;
  map.set(key, choose ? value + 1 : value + 2);
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto& functions = prepared.program.functions;
  const auto found = std::ranges::find(functions, "update", &ir::Function::name);
  ASSERT_NE(found, functions.end());
  const auto lookups = FindMapLookups(*found, functions);
  EXPECT_EQ(std::ranges::count_if(lookups, [](const auto& lookup) { return lookup.has_value(); }),
            1);
}

TEST(MapAnalysisTest, RejectsChangedInputsEffectsAndLookupPathsThatDoNotDominateStores) {
  for (const std::string_view body : {
           "const value = map.get(key) ?? 0; touch(); map.set(key, value + 1);",
           "let name = key; const value = map.get(name) ?? 0; name = 'other'; map.set(name, "
           "value);",
           "let target = map; const value = target.get(key) ?? 0; target = other; target.set(key, "
           "value);",
           "const value = map.get(key) ?? 0; map.clear(); map.set(key, value);",
           "let value = 0; if (choose) { value = map.get(key) ?? 0; } map.set(key, value);",
       }) {
    SCOPED_TRACE(body);
    const auto prepared = ir::test::Prepare(
        "external func touch(): void; export func update(map: Map<string,int>, "
        "other: Map<string,int>, key: string, choose: bool): void {" +
        std::string(body) + "}");
    ASSERT_TRUE(prepared.Succeeded());
    const auto& functions = prepared.program.functions;
    const auto found = std::ranges::find(functions, "update", &ir::Function::name);
    ASSERT_NE(found, functions.end());
    const auto lookups = FindMapLookups(*found, functions);
    EXPECT_TRUE(
        std::ranges::none_of(lookups, [](const auto& lookup) { return lookup.has_value(); }));
  }
}

}  // namespace
}  // namespace vela::compiler::native
