#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/ast_clone.h"
#include "compiler/frontend/checker_test_support.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"

namespace vela::frontend {
namespace {

TEST(MapLiteralParserTest, PreservesEntriesLocationsAndClonesNestedExpressions) {
  const auto parsed =
      ParseSource("func f(): void { const m = {key: [1], true ? 'a' : 'b': {'inner': 2},}; }");
  ASSERT_TRUE(parsed.diagnostics.empty()) << DiagnosticsText(parsed.diagnostics);
  const auto copy = CloneProgram(parsed.program, {});
  const auto& map = *copy.functions.front().body->statements.front()->expression;
  EXPECT_EQ(map.kind, Expression::Kind::MapLiteral);
  ASSERT_EQ(map.entries.size(), 2U);
  const auto& first = map.entries.front();
  EXPECT_EQ(first.key->kind, Expression::Kind::Name);
  EXPECT_EQ(first.key->text, "key");
  EXPECT_EQ(first.key->location.column, 29U);
  EXPECT_EQ(first.value->kind, Expression::Kind::ArrayLiteral);
  const auto& second = map.entries.back();
  EXPECT_EQ(second.key->kind, Expression::Kind::Conditional);
  EXPECT_EQ(second.value->kind, Expression::Kind::MapLiteral);
}

TEST(MapLiteralParserTest, RetainsKnownKeysAndValuesInIncompleteEntries) {
  const auto parsed = ParseSource("func f(): void { const m = {'a': 1, key:");
  ASSERT_FALSE(parsed.diagnostics.empty());
  ASSERT_EQ(parsed.program.functions.size(), 1U);
  const auto& map = *parsed.program.functions.front().body->statements.front()->expression;
  ASSERT_EQ(map.entries.size(), 2U);
  EXPECT_EQ(map.entries.front().value->text, "1");
  EXPECT_EQ(map.entries.back().key->text, "key");
  EXPECT_EQ(map.entries.back().value, nullptr);
}

class MapLiteralSyntaxErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(MapLiteralSyntaxErrorTest, RejectsMalformedOrExcludedSyntax) {
  const auto parsed = ParseSource("func f(): void { const m = " + std::string(GetParam()) + "; }");
  EXPECT_FALSE(parsed.diagnostics.empty());
}

INSTANTIATE_TEST_SUITE_P(MapSyntax,
                         MapLiteralSyntaxErrorTest,
                         testing::Values("{'a' 1}",
                                         "{: 1}",
                                         "{'a':}",
                                         "{,'a':1}",
                                         "{'a':1,,}",
                                         "{name}",
                                         "{...other}",
                                         "<string,int>{'a':1}",
                                         "{'a':1"));

TEST(MapLiteralParserTest, LambdaBraceRemainsABlockAndMapRequiresParentheses) {
  const auto block = ParseSource("func f(): void { const operation = () => {}; }");
  ASSERT_TRUE(block.diagnostics.empty());
  const auto& lambda = *block.program.functions.front().body->statements.front()->expression;
  EXPECT_EQ(lambda.function->body->kind, Statement::Kind::Block);
  for (const auto* key : {"'key'", "key", "key()"}) {
    const auto invalid =
        ParseSource("func f(): void { const operation = () => {" + std::string(key) + ": 1}; }");
    EXPECT_TRUE(std::ranges::any_of(invalid.diagnostics, [](const auto& diagnostic) {
      return diagnostic.message.contains(
          "map literal in a lambda expression body requires parentheses");
    })) << DiagnosticsText(invalid.diagnostics);
  }
  EXPECT_TRUE(ParseSource("func f(): void { const operation = () => ({'key': 1}); "
                          "({'key': 1}); {} }")
                  .diagnostics.empty());
}

class MapLiteralTypeErrorTest
    : public testing::TestWithParam<std::tuple<std::string_view, std::string_view>> {};

TEST_P(MapLiteralTypeErrorTest, ReportsTheEntryOrMissingContext) {
  const auto& parameters = GetParam();
  const std::string_view body = std::get<0>(parameters);
  const std::string_view expected = std::get<1>(parameters);
  const auto checked =
      CheckTestSource("func nothing(): void {} func main(): void { " + std::string(body) + " }");
  EXPECT_FALSE(checked.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [&](const auto& diagnostic) {
    return diagnostic.message.contains(expected);
  })) << DiagnosticsText(checked.diagnostics);
}

INSTANTIATE_TEST_SUITE_P(
    MapTypes,
    MapLiteralTypeErrorTest,
    testing::Values(
        std::tuple{"const m = {};", "empty map requires an expected map type"},
        std::tuple{"const m: dynamic = {};", "empty map requires an expected map type"},
        std::tuple{"const m = {null: 1};", "null map key requires an expected nullable type"},
        std::tuple{"const m = {1: null};", "null map value requires an expected nullable type"},
        std::tuple{"const m = {1: 1, 2.0: 2};", "map key type does not match 'int'"},
        std::tuple{"const m = {1: 1, 2: 2.0};", "map value type does not match 'int'"},
        std::tuple{"const m: Map<int, int> = {'x': 1};", "map key type does not match 'int'"},
        std::tuple{"const m: Map<int, int> = {1: 'x'};", "map value type does not match 'int'"},
        std::tuple{"const m = {nothing(): 1};", "map key must be a value"},
        std::tuple{"const m = {1: nothing()};", "map value must be a value"},
        std::tuple{"const m = {missing: 1};", "unknown"},
        std::tuple{"const m = {1: 1, null: 2};", "map key type does not match"},
        std::tuple{"const m: Map<string, dynamic> = {'x': 1}; "
                   "const n: Map<string, int> = m;",
                   "cannot"}));

TEST(MapLiteralTypeTest, UsesAliasesNestedContextsAndSharedGenericInference) {
  const auto checked = CheckTestSource(R"(
type Values<T> = Map<string, T>;
func identity<T>(value: T): T { return value; }
func apply<T>(values: Map<string, T>, fallback: T): T { return fallback; }
func make<K, V>(key: K, value: V): Map<K, V> { return {key: value}; }
func callbacks<T>(operations: Map<string, (T) => T>, value: T): T { return value; }
func main(): void {
  const empty: Values<int>? = {};
  const nested: Map<string, int[][]> = {'items': [[], [1]]};
  const nullable: Map<string?, int?> = {null: null, 'key': 1};
  const erased: Map<dynamic, dynamic> = {1: 'one', 'two': 2, null: true};
  const result = apply({}, 42);
  const operations = callbacks({'next': value => value + 1, 'same': identity}, 41);
  const returned: Values<int> = identity({});
  const made = make('answer', result);
}
)");
  EXPECT_TRUE(checked.Succeeded()) << DiagnosticsText(checked.diagnostics);
  EXPECT_TRUE(checked.diagnostics.empty()) << DiagnosticsText(checked.diagnostics);
}

TEST(MapLiteralTypeTest, ChecksUnusedGenericDefinitionsAndInitializationInsideEntries) {
  EXPECT_FALSE(CheckTestSource("func invalid<T>(key: T): Map<T, int> { return {key: 'wrong'}; }")
                   .Succeeded());
  EXPECT_FALSE(CheckTestSource("class Box { value: int; constructor() { const m = {'self': this}; "
                               "this.value = 42; } }")
                   .Succeeded());
}

}  // namespace
}  // namespace vela::frontend
