#include <array>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/frontend/modules/source_loader.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"

namespace vela::testing {
namespace {

class ScriptMatcherTest : public ::testing::TestWithParam<std::string_view> {};

TEST_P(ScriptMatcherTest, MatchesAgainstIndependentExpectedResultsUnderGc) {
  const auto inputs = std::to_array<frontend::SourceInput>({
      {.path = "matcher.vela", .text = GetParam()},
  });
  runtime::Isolate isolate(1);
  const auto compiled = compiler::test::CompileCleanModules(inputs, "matcher.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = compiler::test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

INSTANTIATE_TEST_SUITE_P(DeepAndExtensible,
                         ScriptMatcherTest,
                         ::testing::Values(
                             R"(import 'sdk:test';
func main(): int {
  expect(42, 42);
  expect(42, isNot(equals(42.0)));
  expect(null, isNull);
  expect(true, isTrue);
  expect(false, isFalse);
  expect(42, allOf([greaterThan(41), lessThan(43)]));
  expect(42.0, closeTo(42.01, 0.02));
  expect('Vela', contains('el'));
  expect([1,2,3], contains(2));
  expect([1,2,3], hasLength(3));
  expect([], isEmpty);
  expect([1], isNotEmpty);
  return 42;
})",
                             R"(import 'sdk:test';
func main(): int {
  const first: dynamic[] = [null, [42]];
  const second: dynamic[] = [null, [42]];
  first[0] = first; second[0] = second;
  expect(first, equals(second));
  second[1] = [43];
  expect(first, isNot(equals(second)));
  const shared = [42];
  expect([shared, shared], equals([[42], [42]]));
  expect(shared, same(shared));
  expect(shared, isNot(same([42])));
  return 42;
})",
                             R"(import 'sdk:test';
func main(): int {
  let actual: dynamic = 42;
  let expected: dynamic = 42;
  for (let index = 0; index < 2048; index++) {
    const nextActual: dynamic[] = [actual];
    const nextExpected: dynamic[] = [expected];
    actual = nextActual; expected = nextExpected;
  }
  expect(actual, equals(expected));
  return 42;
})",
                             R"(import 'sdk:test';
func main(): int {
  const actual = Map<string, dynamic>();
  actual.set('items', [1,2,3]); actual.set('name', 'Vela');
  const expected = Map<string, dynamic>();
  expected.set('name', 'Vela'); expected.set('items', [1,2,3]);
  expect(actual, equals(expected));
  expected.set('items', [1,4,3]);
  const result = equals(expected).match(actual);
  if (result.success) { return 0; }
  const difference = result.difference;
  if (difference == null || difference.path != "['items'][1]" || difference.actual != '2') { return 1; }
  return 42;
})",
                             R"(import 'sdk:test';
func main(): int {
  const actual = Set<dynamic>(); actual.add(42); actual.add(actual);
  const expected = Set<dynamic>(); expected.add(expected); expected.add(42);
  expect(actual, equals(expected));
  expect([1,1,2], unorderedEquals([2,1,1]));
  expect([1,2,2], isNot(unorderedEquals([1,1,2])));
  const ordered = Set<int>(); ordered.add(1); ordered.add(2);
  const reversed = Set<int>(); reversed.add(2); reversed.add(1);
  expect(ordered, orderedEquals([1,2]));
  expect(reversed, isNot(orderedEquals(ordered)));
  expect(42, isNot(orderedEquals([42])));
  return 42;
})",
                             R"(import 'sdk:test';
func main(): int {
  let comparisons = 0;
  const broad = predicate((value: dynamic): bool => { comparisons++; return value is int; }, 'integer');
  const narrow = predicate((value: dynamic): bool => { comparisons++; return value == 1; }, 'one');
  expect([1,2], unorderedEquals([broad, narrow]));
  if (comparisons != 4) { return 0; }
  return 42;
})",
                             R"(import 'sdk:test';
class DomainFailure { value: int; }
class Custom implements Matcher {
  match(actual: dynamic): MatchResult {
    return MatchResult(success: actual == 42, description: 'the answer');
  }
}
func main(): int {
  expect(42, Custom());
  const original = DomainFailure(value: 42);
  expect(() => { throw original; }, throwsA(same(original)));
  expect(() => { throw original; }, throwsA(predicate((error: dynamic): bool => error is DomainFailure, 'domain failure')));
  return 42;
})",
                             R"(import 'sdk:test';
class DomainFailure {}
class Throwing implements Matcher {
  original: DomainFailure;
  match(actual: dynamic): MatchResult { throw this.original; }
}
func main(): int {
  const original = DomainFailure();
  try { expect(42, Throwing(original: original)); }
  catch (error: dynamic) { if (error == original) { return 42; } }
  return 0;
})",
                             R"(import 'sdk:test';
func main(): int {
  let actual = '';
  let expected = '';
  for (let index = 0; index < 1024; index++) { actual += 'a'; expected += 'a'; }
  expected += 'b';
  try { expect(actual, equals(expected), reason: 'must compare beyond display budget'); }
  catch (error: TestFailure) {
    if (!error.message.includes('must compare beyond display budget')) { return 0; }
    return 42;
  }
  return 1;
})"));

}  // namespace
}  // namespace vela::testing
