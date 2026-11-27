#include <array>
#include <span>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "compiler/test_support/checker_test_support.h"
#include "compiler/test_support/execution_expectations.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

constexpr std::string_view kIntegerPredicate = R"(
func isInteger(value: dynamic): bool {
  if (value is int) { return true; }
  return false;
}
)";

class DynamicArrayExecutionTest : public testing::TestWithParam<std::string_view> {};

void ExpectRejectedStatements(std::span<const std::string_view> statements) {
  for (const auto statement : statements) {
    const auto checked =
        frontend::CheckTestSource("func main(): void { " + std::string(statement) + " }");
    EXPECT_FALSE(checked.Succeeded()) << statement;
  }
}

INSTANTIATE_TEST_SUITE_P(SearchPaths,
                         DynamicArrayExecutionTest,
                         testing::Values(
                             R"(func main(): int {
  const values = [1, 2, 3, 4]; let calls = 0;
  const selected: int[] = values.filter((value, index, receiver) => {
    calls++; if (index == 0) { receiver[1] = 6; receiver.reserve(100); }
    return value % 2 == 0;
  });
  if (calls != 4 || selected.join() != '6,4' || values.join() != '1,6,3,4') { return 0; }
  selected[0] = 42; if (values[1] != 6) { return 0; }
  calls = 0;
  const found: int? = values.find((value, index, receiver) => {
    calls++; if (index == 1) { receiver[index] = 99; } return value == 6;
  });
  if (found != 6 || calls != 2 || values[1] != 99) { return 0; }
  const empty: int[] = [];
  if (empty.find(() => true) != null || empty.filter(() => true).length != 0 ||
      empty.includes(1) || values.find(value => value == -1) != null) { return 0; }
  return 42;
})",
                             R"(class Item { value: int; }
func main(): int {
  const items = [Item(value: 1), Item(value: 42)];
  const retained = items.filter(item => item.value == 42);
  const found = items.find(item => item.value == 42);
  if (found == null || found != retained[0] || !items.includes(found) ||
      items.includes(Item(value: 42))) { return 0; }
  items.clear();
  for (let i = 0; i < 300; i++) { const garbage = [Item(value: i)]; }
  if (retained[0].value != 42 || found.value != 42) { return 0; }
  const nested: int[][] = [[1], [42]];
  if (!nested.includes(nested.filter(value => value[0] == 42)[0]) ||
      !nested.includes(nested[1]) || nested.includes([42])) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const nullable: int?[] = [null, 42, null];
  const selected: null[] = nullable.filter(value => value == null);
  if (selected.length != 2 || selected[0] != null || nullable.find(value => value == 42) != 42 ||
      nullable.find(value => value == null) != null || !nullable.includes(null) ||
      nullable.includes(null, 3)) { return 0; }
  const flags = [false, true];
  if (!flags.includes(false) || !flags.filter(value => value)[0] || flags.find(value => !value) != false) {
    return 0;
  }
  const nan = Math.pow(-1.0, 0.5);
  const doubles = [nan, -0.0, 42.5];
  const tagged: double?[] = [null, nan, -0.0];
  if (!doubles.includes(nan) || !doubles.includes(0.0) || doubles.includes(nan, 1) ||
      !tagged.includes(nan) || !tagged.includes(null) || !tagged.includes(0.0)) { return 0; }
  const text: string?[] = [null, '你好', 'a\u0000b'];
  if (!text.includes(null) || !text.includes('你' + '好') || !text.includes('a\u0000b') ||
      text.includes('a')) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const values = [1, 2, 3];
  if (!values.includes(1, -9223372036854775808) || !values.includes(3, -1) ||
      values.includes(2, -1) || values.includes(1, 3) || values.includes(1, 9223372036854775807) ||
      !values.includes(fromIndex: 1, searchElement: 2)) { return 0; }
  const includes: (int) => bool = values.includes;
  const from: (int, int) => bool = values.includes;
  const filter: ((int) => bool) => int[] = values.filter;
  const find: ((int) => bool) => int? = values.find;
  if (!includes(2) || from(2, 2) || filter(value => value > 1).join() != '2,3' ||
      find(value => value == 3) != 3) { return 0; }
  let receiver = [1]; const original = receiver;
  if (receiver.includes((receiver = [42])[0]) || original[0] != 1) { return 0; }
  return 42;
})",
                             R"(enum Color { Red, Blue }
func main(): int {
  const values = [Color.Red, Color.Blue];
  const selected = values.filter(value => value == Color.Blue);
  if (selected.length != 1 || !selected.includes(Color.Blue) ||
      values.find(value => value == Color.Blue) != Color.Blue) { return 0; }
  const erased: dynamic = values;
  const filter = erased.filter; const find = erased.find; const includes = erased.includes;
  const result: Color[] = filter((value: Color) => value == Color.Blue);
  if (!includes(Color.Blue) || includes(Color.Red, 1) || result[0] != Color.Blue ||
      find((value: Color) => value == Color.Blue) != Color.Blue) { return 0; }
  const mixed: dynamic[] = [1, 1.0, 'x', null];
  if (!mixed.includes(1) || mixed.includes(1, 1) || !mixed.includes(1.0, 1) ||
      !mixed.includes(null) || mixed.filter(value => value is int).length != 1) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const values = [1, 2, 3]; const erased: dynamic = values;
  const filtered: int[] = erased.filter((value: int, index: int, receiver: int[]) => {
    receiver.reserve(100); return index > 0 && value > 1;
  });
  if (filtered.join() != '2,3' || erased.find((value: int) => value == 2) != 2 ||
      erased.find((value: int) => false) != null || !erased.includes(2)) { return 0; }
  let caught = 0;
  try { erased.filter((value: int): dynamic => 'invalid'); } catch (error: Error) { caught++; }
  try { erased.find((value: int): dynamic => 1); } catch (error: Error) { caught++; }
  try { erased.includes('wrong'); } catch (error: Error) { caught++; }
  if (caught != 3) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  let caught = 0;
  const a = [1];
  try { a.filter(value => { a.push(2); a.pop(); return true; }); }
  catch (error: Error) { if (error.code == 'ConcurrentModification') { caught++; } }
  const b = [1];
  try { b.find(value => { b.clear(); return true; }); }
  catch (error: Error) { if (error.code == 'ConcurrentModification') { caught++; } }
  const c = [1]; const erased: dynamic = c;
  try { erased.filter((value: int) => { c.push(2); return false; }); }
  catch (error: Error) { if (error.code == 'ConcurrentModification') { caught++; } }
  const d = [1]; const other: dynamic = d;
  try { other.find((value: int) => { d.push(2); return true; }); }
  catch (error: Error) { if (error.code == 'ConcurrentModification') { caught++; } }
  if (caught != 4 || a.length != 1 || b.length != 0 || c.length != 2 || d.length != 2) { return 0; }
  return 42;
})"));

TEST(ArraySearchTest, NullableReferenceStoragePreservesReflectionIdentityEquality) {
  constexpr std::string_view kSource = R"(
[Reflect]
class Item { value: int = 42; }
func main(): int {
  const item = Item();
  const objects: Item?[] = [null, item];
  if (!objects.includes(null) || !objects.includes(item) || objects.includes(Item())) { return 0; }
  const first = Reflect.typeInfo<Item>().findField('value');
  const second = Reflect.typeInfo<Item>().findField('value');
  if (first == null || second == null) { return 0; }
  const fields: FieldInfo<Item>?[] = [null, first];
  if (!fields.includes(null) || !fields.includes(first)) { return 0; }
  const identities: MemberId?[] = [null, first.id];
  if (!identities.includes(null) || !identities.includes(second.id) ||
      identities.includes(second.id, 2)) { return 0; }
  const mixed: dynamic[] = [Math.pow(-1.0, 0.5), -0.0, 42];
  if (!mixed.includes(Math.pow(-1.0, 0.5)) || !mixed.includes(0.0) || mixed.includes(42.0)) { return 0; }
  return 42;
}
)";
  test::ExpectRunAnswer(kSource);
  test::ExpectRestoredAnswer(kSource);
}

TEST(ArraySearchTest, RejectsInvalidPredicatesArgumentsAndNonnullableFindResults) {
  const auto invalid = std::to_array<std::string_view>({
      "const a = [1]; a.filter(value => value);",
      "const a = [1]; a.find(value => value);",
      "const a = [1]; const value: int = a.find(value => true);",
      "const a = [1]; const result: double[] = a.filter(value => true);",
      "const a = [1]; a.includes(1.0);",
      "const a = [1]; a.includes(null);",
      "const a = [1]; a.includes(1, 0.0);",
      "const a = [1]; a.includes();",
      "const a = [1]; a.filter();",
      "const a = [1]; a.find();",
      "const a = [1]; a.filter((value: string) => true);",
      "const a: dynamic[] = [1]; const b: int[] = a.filter(value => { value = 1; return value is "
      "int; });",
      "const a: dynamic[] = [1]; const b: int[] = a.filter(value => { if (value is int) { return "
      "true; } return true; });",
      "const a: dynamic[] = [1]; const flag: dynamic = true; const b: int[] = a.filter(value => { "
      "if (value is int) { return true; } return flag; });",
  });
  ExpectRejectedStatements(invalid);
}

TEST(ArraySearchTest, LazyPrivatePredicatesSurviveGenericMethodTableGrowth) {
  constexpr std::string_view kSource = R"(
class Predicates {
  #identity<T>(value: T): T { return value; }
  #isInteger(value: dynamic): bool {
    this.#identity(1);
    this.#identity('text');
    this.#identity(false);
    this.#identity(1.0);
    this.#identity([1]);
    return value is int;
  }
  run(values: dynamic[]): int {
    const predicate = this.#isInteger;
    const selected: int[] = values.filter(predicate);
    return selected[0];
  }
}
func main(): int { return Predicates().run([42, 'text']); }
)";
  test::ExpectRunAnswer(kSource);
  test::ExpectRestoredAnswer(kSource);
}

TEST(ArraySearchTest, NarrowsEverySuccessfulReturnAcrossBranchesAndForwardPredicates) {
  test::ExpectRunAnswer(R"(
func main(): int {
  const values: dynamic[] = [42, 'x', -1, null];
  const integers: int[] = values.filter(value => {
    if (value == null) { return false; }
    if (value is int) { if (value > 0) { return true; } return false; }
    return false;
  });
  const predicate = (value: dynamic): bool => {
    if (!(value is int)) { return false; }
    if (value < 0) { return false; }
    return true;
  };
  const saved: int[] = values.filter(predicate);
  const named: int[] = values.filter(isInteger);
  const forwarded: int[] = values.filter(value => {
    if (isInteger(value)) { return true; }
    return false;
  });
  const found: int? = values.find(value => {
    if (value is int) { return value > 0; }
    return false;
  });
  const nullable: int?[] = [null, 42];
  const present: int[] = nullable.filter(value => {
    if (value == null) { return false; }
    return value > 0;
  });
  if (integers[0] != 42 || saved[0] != 42 || named.length != 2 || forwarded.length != 2 ||
      present[0] != 42 || found == null) { return 0; }
  return found;
}
)" + std::string(kIntegerPredicate));
}

TEST(ArraySearchTest, DynamicFilterAllocatesTheProvenElementLayout) {
  test::ExpectRunAnswer(R"(
class Item { value: int; }
func main(): int {
  const values: dynamic[] = [Item(value: 42), 1, null];
  const receiver: dynamic = values;
  const selected: Item[] = receiver.filter((value: dynamic) => {
    if (value is Item) { return true; }
    return false;
  }) as Item[];
  const integers: int[] = receiver.filter((value: dynamic) => value is int) as int[];
  if (integers.length != 1 || integers[0] != 1 || selected.length != 1) { return 0; }
  return selected[0].value;
}
)");
}

TEST(ArraySearchTest, GenericAndClassPredicatesPreserveDeclaredConstraints) {
  test::ExpectRunAnswer(R"(
class Base { value: int; }
class First extends Base {}
class Second extends Base {}
func main(): int {
  const values: int?[] = [null, 42];
  const present: int[] = nonnull<int>(values);
  const mixed: dynamic[] = [42, 'x', null];
  const typed: int[] = select<int>(mixed);
  const named: int[] = mixed.filter(isInteger);
  const either: int[] = mixed.filter(value =>
      (value is int && value > 0) || (value is int && value < 0));
  const objects: dynamic[] = [First(value: 40), Second(value: 2), 0];
  const bases: Base[] = objects.filter(value => {
    if (value is First) { return true; }
    if (value is Second) { return true; }
    return false;
  });
  if (present[0] != 42 || typed[0] != 42 || named[0] != 42 || either[0] != 42 ||
      !use<int>(42) || bases.length != 2) { return 0; }
  return bases[0].value + bases[1].value;
}
func nonnull<T>(values: T?[]): T[] { return values.filter(value => value != null); }
func select<T>(values: dynamic[]): T[] { return values.filter(value => value is T); }
func use<T>(value: T): bool {
  const values: dynamic[] = [value]; return values.some(isInteger);
}
)" + std::string(kIntegerPredicate));
}

TEST(ArraySearchTest, DoesNotAssumeVirtualOverridesPreserveAnInferredPredicate) {
  const auto checked = frontend::CheckTestSource(R"(
class Guard { check(value: dynamic): bool { return value is int; } }
class AcceptAll extends Guard {
  override check(value: dynamic): bool { return true; }
}
func main(): void {
  const guard: Guard = AcceptAll();
  const values: dynamic[] = ['text'];
  const integers: int[] = values.filter(guard.check);
}
)");
  EXPECT_FALSE(checked.Succeeded());
}

TEST(ArraySearchTest, LaterArgumentsCannotApplyAnOldValueProofToANewBinding) {
  test::ExpectRunAnswer(R"(
func matches(value: dynamic, other: dynamic): bool { return value is int && other != null; }
func main(): int {
  let value: dynamic = 42;
  if (matches(value, value = 'x')) { return (value.length as int) + 41; }
  return 0;
}
)");
}

TEST(ArraySearchTest, DynamicPredicatesCannotWidenTheSourceArrayElementType) {
  test::ExpectRunAnswer(R"(
class Base { value: int; }
class Child extends Base {}
func main(): int {
  const values: Child[] = [Child(value: 42)];
  const erased: dynamic = values;
  const result: Child[] = erased.filter((value: dynamic) => value is Base) as Child[];
  return result[0].value;
}
)");
}

TEST(ArraySearchTest, PropagatesPredicateFailuresAndDoesNotCallPastAFindMatch) {
  test::ExpectRunAnswer(R"(
func main(): int {
  const values = [1, 2]; let visits = 0; let divisor = 0;
  try {
    values.filter(value => { visits++; return value / divisor > 0; });
    return 0;
  } catch (error: Error) {}
  if (visits != 1 || values.join() != '1,2') { return 0; }
  const first = values.find(value => {
    if (value == 2) { return value / divisor > 0; }
    return true;
  });
  if (first != 1) { return 0; }
  return 42;
}
)");
}

TEST(ArraySearchTest, InvalidPredicateArgumentsProduceDiagnosticsInsteadOfFlowFacts) {
  for (const auto* const call : {
           "matches(unknown: value)",
           "matches(value: value, value: value)",
           "matches(value, value)",
       }) {
    const auto source = std::string(
                            "func matches(value: dynamic): bool { return value is int; } "
                            "func main(): void { const value: dynamic = 42; if (") +
                        call + ") { print(value); } }";
    EXPECT_FALSE(frontend::CheckTestSource(source).Succeeded()) << call;
  }
}

TEST(ArraySearchTest, NullSafeCallsSkipArgumentsAndPreserveNullableResultTypes) {
  test::ExpectRunAnswer(R"(
let argumentCalls = 0;
let callbackCalls = 0;
func predicate(): (int) => bool {
  argumentCalls++;
  return (value: int): bool => { callbackCalls++; return value > 0; };
}
func needle(): int { argumentCalls++; return 42; }
func check(values: int[]?): int {
  argumentCalls = 0; callbackCalls = 0;
  const filtered: int[]? = values?.filter(predicate());
  const found: int? = values?.find(predicate());
  const included: bool? = values?.includes(needle());
  if (values == null) {
    if (filtered != null || found != null || included != null || argumentCalls != 0 || callbackCalls != 0) {
      return 0;
    }
  } else {
    if (filtered == null || filtered[0] != 42 || found != 42 || included != true ||
        argumentCalls != 3 || callbackCalls != 2) {
      return 0;
    }
  }
  return 21;
}
func main(): int { return check(null) + check([42]); }
)");
}

TEST_P(DynamicArrayExecutionTest, RunsTypedGrowthAndMutationThroughNativeImagesAndGc) {
  runtime::Isolate isolate(1);
  const auto program = test::ReinstantiateSource(GetParam(), isolate);
  ASSERT_TRUE(program) << program.error();
  auto& heap = isolate.heap();
  heap.FinishCollection();
  heap.Collect();
  test::ExpectStableAnswer(**program);
}

TEST(DynamicArrayTest, IntegerOrdersPreserveAliasesBoundsAndComparatorFailures) {
  test::ExpectRunAnswer(R"(
func main(): int {
  const values = [9223372036854775807, -1, 0, -9223372036854775808, 42, -1];
  const alias = values;
  const sorted = values.sort((a, b) => a < b ? -1 : (a > b ? 1 : 0));
  if (sorted[0] != -9223372036854775808 || alias[0] != -9223372036854775808 ||
      values.join() != '-9223372036854775808,-1,-1,0,42,9223372036854775807') { return 0; }
  values.sort((a, b) => a > b ? -7 : (a < b ? 9 : 0));
  if (alias.join() != '9223372036854775807,42,0,-1,-1,-9223372036854775808') { return 0; }
  const empty: int[] = []; empty.sort((a, b) => a < b ? -1 : (a > b ? 1 : 0));
  if (empty.length != 0) { return 0; }
  const single = [42]; single.sort((a, b) => a > b ? -1 : (a < b ? 1 : 0));
  if (single[0] != 42) { return 0; }
  const unchanged = [9223372036854775807, -9223372036854775808];
  try { unchanged.sort((a, b) => a - b); return 0; }
  catch (error: Error) { if (error.code != 'IntegerOverflow') { return 0; } }
  if (unchanged[0] != 9223372036854775807) { return 0; }
  let calls = 0;
  const counted = [2, 1];
  counted.sort((a, b) => { calls++; return a < b ? -1 : (a > b ? 1 : 0); });
  if (calls == 0 || counted[0] != 1) { return 0; }
  return 42;
}
)");
}

INSTANTIATE_TEST_SUITE_P(ProductionPaths,
                         DynamicArrayExecutionTest,
                         testing::Values(
                             R"(func main(): int {
  const typed = [1, 2, 3];
  const values: dynamic = typed;
  const splice = values.splice;
  if (values.splice().length != 0 || typed.length != 3) { return 0; }
  if (splice().length != 0 || typed.length != 3) { return 0; }
  if (values.splice(1).join() != '2,3' || typed.join() != '1') { return 0; }
  typed.push(2, 3);
  if (splice(1, 0, 42).length != 0 || typed.join() != '1,42,2,3') { return 0; }
  if (splice(2, 1).join() != '2' || typed.join() != '1,42,3') { return 0; }
  return typed[1];
})",
                             R"(func main(): int {
  const values: int[] = []; const alias = values;
  values.reserve(4096);
  for (let i = 0; i < 4096; i = i + 1) { values.push(i); }
  if (alias.length != 4096 || alias[4095] != 4095) { return 0; }
  values.splice(1, 0, 42);
  if (alias[1] != 42 || values.splice(1, 1)[0] != 42) { return 0; }
  const last = values.pop(); if (last == null || last != 4095) { return 0; }
  const capacity = values.capacity; values.clear();
  if (values.capacity != capacity || values.length != 0 || values.pop() != null) { return 0; }
  if (values.push(42) != 1) { return 0; }
  return alias[0];
})",
                             R"(func main(): int {
  const numbers: int?[] = []; numbers.push(null); numbers.unshift(42);
  const first = numbers.shift(); if (first == null) { return 0; }
  if (numbers.pop() != null || numbers.pop() != null) { return 0; }
  const flags: bool[] = []; flags.push(true); flags.unshift(false); flags.reverse();
  const flag = flags.pop(); if (flag == null || flag) { return 0; }
  const doubles: double[] = []; doubles.push(1.5); doubles.reserve(100); doubles.fill(2.5);
  const value = doubles.pop(); if (value == null || value != 2.5) { return 0; }
  return first;
})",
                             R"(class Item { value: int; }
func main(): int {
  const items: Item[] = [];
  for (let i = 0; i < 1000; i = i + 1) { items.push(Item(value: i)); }
  const old = items[42]; const copy = items.slice(40, 44); items.clear();
  if (copy[2] != old) { return 0; }
  const nested: Item[][] = []; nested.push(copy); nested.reserve(100);
  const popped = nested.pop(); if (popped == null) { return 0; }
  return popped[2].value;
})",
                             R"(func main(): int {
  let values = [1]; const original = values;
  original.push((values = [40])[0]);
  original.splice(0, 0, (values = [42])[0]);
  if (original.length != 3 || values.length != 1 || original[2] != 40) { return 0; }
  return original.splice(0, 1)[0];
})",
                             R"(class Box { value: int; } func main(): int {
  const values: ((int) => int)[] = [];
  for (let i = 0; i < 100; i = i + 1) {
    const offset = i; values.push((x: int): int => offset + x);
  }
  const operation = values.pop(); if (operation == null) { return 0; }
  const data: Box[] = []; data.push(Box(value: 42));
  const result = data.pop(); if (result == null || operation(1) != 100) { return 0; }
  return result.value;
})",
                             R"(func main(): int {
  const words: string[] = [];
  for (let i = 0; i < 1000; i = i + 1) { words.push('word_${i}'); }
  words.reserve(2000); words.unshift('hello');
  if (words.shift() != 'hello' || words.pop() != 'word_999') { return 0; }
  const copy = words.slice(); copy.clear();
  if (words.length != 999 || words.slice(0, 2).join('/') != 'word_0/word_1') { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const values = [1, 2, 3];
  values.forEach(value => { values.reserve(100); values[0] = 42; });
  values.sort((left, right) => { values.reserve(200); return left - right; });
  return values[2];
})",
                             R"(func main(): int {
  const values: int[] = []; let caught = 0;
  try { print(values[-1]); } catch (error: Error) { caught = caught + 1; }
  try { print(values[0]); } catch (error: Error) { caught = caught + 1; }
  try { values.reserve(-1); } catch (error: Error) { caught = caught + 1; }
  try { values[0] = 1; } catch (error: Error) { caught = caught + 1; }
  try { print(values[4294967296]); } catch (error: Error) { caught = caught + 1; }
  if (caught == 5 && values.length == 0) { return 42; } return 0;
})"));

TEST(DynamicArrayTest, RejectsImplicitConversionsAndReadonlyLengthOrCapacityWrites) {
  const auto invalid = std::to_array<std::string_view>({
      "const values: int[] = []; values.push(1.0);",
      "const values: double[] = []; values.splice(0, 0, 1);",
      "const values: int[] = []; values.push(null);",
      "const values: int[] = []; const value: int = values.pop();",
      "const values: int[] = []; values.length = 42;",
      "const values: int[] = []; values.capacity = 42;",
      "const values: int[] = []; values.reserve(1.0);",
      "const values: int[] = []; const other: double[] = values;",
      "const values = [1]; values.insert(0, 2);",
      "const values = [1]; values.removeAt(0);",
      "const values = [1]; values.copy();",
      "const values = [1]; values.push(items: 2);",
      "const values = [1]; values.reduce(0, (a, b) => a + b);",
      "const values = [1]; values.reduce((a, b) => a + b);",
      "const values: int[] = []; values.reduce((a, b) => a + b);",
      "const values = [1]; values.map((a, b, c, d) => a);",
  });
  ExpectRejectedStatements(invalid);
}

INSTANTIATE_TEST_SUITE_P(JavaScriptContracts,
                         DynamicArrayExecutionTest,
                         testing::Values(
                             R"(func main(): int {
  const a = [3];
  if (a.push() != 1 || a.unshift() != 1 || a.push(4, 5) != 3) { return 0; }
  if (a.unshift(1, 2) != 5 || a.join() != '1,2,3,4,5') { return 0; }
  if (a.shift() != 1 || a.pop() != 5) { return 0; }
  a.clear(); if (a.shift() != null || a.pop() != null) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a = [0, 1, 2, 3]; const alias = a;
  const removed = a.splice(-3, 2, 8, 9, 10);
  if (removed.join() != '1,2' || alias.join() != '0,8,9,10,3') { return 0; }
  removed[0] = 42;
  if (a.splice().length != 0 || a.length != 5) { return 0; }
  if (a.splice(99, 5, 4).length != 0 || a.pop() != 4) { return 0; }
  if (a.splice(-2).join() != '10,3' || a.join() != '0,8,9') { return 0; }
  if (a.splice(-99, -2, 7).length != 0 || a.join() != '7,0,8,9') { return 0; }
  if (a.splice(0, 99).join() != '7,0,8,9' || a.length != 0) { return 0; }
  return removed[0];
})",
                             R"(func main(): int {
  const a: string?[] = ['a', null, 'c'];
  if (a.splice(1, 1, 'b', null).join() != '' || a.join() != 'a,b,,c') { return 0; }
  if (a.shift() != 'a' || a.pop() != 'c') { return 0; }
  const nested = [[1], [2]]; const removed = nested.splice(0, 1, [42]);
  if (removed[0][0] != 1 || nested[0][0] != 42) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  let values = [1]; const original = values; let order = 0;
  const next = (x: int): int => { order = order * 10 + x; values = [99]; return x; };
  original.push(next(2), next(3));
  original.splice(next(0), next(1), next(4), next(5));
  if (order != 230145 || original.join() != '4,5,2,3' || values[0] != 99) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a = [1, 2, 3, 4];
  if (a.slice(-2).join() != '3,4' || a.slice(99).length != 0 ||
      a.slice(3, 1).length != 0 || a.slice(-99, 99).join() != '1,2,3,4') { return 0; }
  const filled = a.fill(8, -3, -1);
  if (filled.join() != '1,8,8,4') { return 0; }
  filled[0] = 5; if (a[0] != 5) { return 0; } filled[0] = 1;
  const reversed = a.fill(9, 3, 1).reverse();
  if (reversed.join() != '4,8,8,1') { return 0; }
  reversed[0] = 5; if (a[0] != 5) { return 0; } reversed[0] = 4;
  if (a.fill(7, end: 2).join() != '7,7,8,1') { return 0; }
  const copy = a.slice(); copy[0] = 42;
  if (a[0] != 7) { return 0; } return copy[0];
})",
                             R"(func main(): int {
  const a = [1, 2, 3]; let calls = 0;
  const sum = a.reduce((total, value, index, array) => {
    if (array[index] != a[index] || index != calls) { return -100; }
    calls = calls + 1; return total + value;
  }, 0);
  if (sum != 6 || calls != 3) { return 0; }
  if (a.reduce((s, v) => s + v.toString(), '') != '123') { return 0; }
  const empty: int[] = [];
  if (empty.reduce((s, v) => s + v, 42) != 42) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a = [10, 20]; let calls = 0;
  a.forEach((value, index, array) => { calls = calls + 1; return array[index] + value; });
  const b = a.map((value, index, array) => value + index + array.length);
  if (calls != 2 || b.join() != '12,23') { return 0; }
  if (!a.some((value, index) => index == 1) || !a.every(() => true) ||
      a.findIndex((value, index, array) => array[index] == 20) != 1) { return 0; }
  return a.map(() => 42)[0];
})",
                             R"(class Item { key: int; id: int; }
func main(): int {
  const a = [Item(key: 1, id: 1), Item(key: 0, id: 2), Item(key: 1, id: 3), Item(key: 0, id: 4)];
  if (a.sort((x, y) => x.key - y.key).map(x => x.id).join() != '2,4,1,3') { return 0; }
  if (a.sort(() => 0).map(x => x.id).join() != '2,4,1,3') { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const numbers = [10, 2, 1];
  const sorted = numbers.sort();
  if (numbers.join() != '1,10,2') { return 0; }
  sorted[0] = 42; if (numbers[0] != 42) { return 0; }
  const text = ['\uE000', '😀', 'a']; text.sort();
  if (text[0] != 'a' || text[1] != '😀') { return 0; }
  const boundaries = ['x\uFFFF', 'x\u{10000}', 'x\uD7FF', 'x\uE000', 'x\u{10FFFF}', 'x', 'x\u0000'];
  boundaries.sort();
  if (boundaries.join('|') != 'x|x\u0000|x\uD7FF|x\u{10000}|x\u{10FFFF}|x\uE000|x\uFFFF') { return 0; }
  const doubles = [2.5, 1.5, 3.5];
  if (doubles.sort((a, b) => a - b).join() != '1.5,2.5,3.5') { return 0; }
  if (doubles.sort(() => Math.pow(-1.0, 0.5)).join() != '1.5,2.5,3.5') { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a = [3, 1, 2];
  a.sort((x, y) => { a[0] = 99; return x - y; });
  if (a.join() != '1,2,3') { return 0; }
  const empty: int[] = []; empty.sort().push(42); if (empty[0] != 42) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a: double?[] = [-0.0, null, 0.000001, 0.0000001, 1e20, 1e21];
  if (a.join() != '0,,0.000001,1e-7,100000000000000000000,1e+21') { return 0; }
  const b = [Math.pow(-1.0, 0.5), Math.exp(1000.0), -Math.exp(1000.0)];
  if (b.join() != 'NaN,Infinity,-Infinity') { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const values: dynamic[] = ['z', 10.0, 2.0, 1.0, null, false, '😀', '\uE000'];
  const erased: dynamic = values;
  erased.sort();
  if (values.join() != '1,10,2,false,,z,😀,\uE000') { return 0; }
  const tied: dynamic[] = [1.0, 1];
  const sort: () => dynamic[] = tied.sort;
  sort();
  if (!(tied[0] is double) || !(tied[1] is int)) { return 0; }
  const numbers = [1e21, 1e20, 1e-7, 1e-6];
  numbers.sort();
  if (numbers.join() != '0.000001,100000000000000000000,1e+21,1e-7') { return 0; }
  return 42;
})"));

class ArrayCallbackMutationTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ArrayCallbackMutationTest, DetectsMutationBeforeUsingCallbackResultsAndVmRemainsReusable) {
  runtime::Isolate isolate(1);
  const auto source = "func main(): void { const values = [3, 1, 2]; " + std::string(GetParam()) +
                      " } func answer(): int { return 42; }";
  const auto compiled = test::CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = test::Execute(*compiled.instance, "main");
  EXPECT_EQ(result.status, ExecutionStatus::ConcurrentModification) << result.message;
  const auto answer = test::Execute(*compiled.instance, "answer");
  ASSERT_TRUE(answer.Succeeded());
  EXPECT_EQ(answer.value.AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(
    Callbacks,
    ArrayCallbackMutationTest,
    testing::Values("values.forEach(value => { values.push(value); });",
                    "values.map(value => { values.clear(); return value; });",
                    "values.some(value => { values.pop(); return true; });",
                    "values.every(value => { values.unshift(value); return false; });",
                    "values.findIndex(value => { values.shift(); return true; });",
                    "values.reduce((total, value) => { values.pop(); return total + value; }, 0);",
                    "values.sort((left, right) => { values.clear(); return left - right; });",
                    "values.map(value => { values.push(value); values.pop(); return value; });"));

}  // namespace
}  // namespace vela::compiler
