#include <algorithm>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "compiler/test_support/execution_expectations.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

class ClassDispatchTest : public testing::TestWithParam<std::string_view> {};

TEST(StructTest, StaticStructFieldsKeepWritableLocationsAndIndependentSnapshots) {
  test::ExpectRunAnswer(R"(
struct Counter { value: int; advance(): void { this.value++; } }
class Holder { static current: Counter = Counter(value: 1); }
func main(): int {
  const snapshot = Holder.current;
  Holder.current.value = 40;
  Holder.current.value++;
  Holder.current.advance();
  return snapshot.value == 1 ? Holder.current.value : -1;
}
)");
}

TEST(StructTest, EmptyNestedValuesPreserveLogicalAndPhysicalFieldOffsets) {
  test::ExpectRunAnswer(R"(
struct Empty {}
[Reflect]
struct Pair { empty: Empty; maybe: Empty?; number: int; text: string; }
[Reflect]
class Holder { empty: Empty; pair: Pair; tail: int; }
func main(): int {
  const pair = Pair(empty: Empty(), maybe: null, number: 20, text: 'kept');
  const holder = Holder(empty: Empty(), pair: pair, tail: 22);
  const boxed: dynamic = pair;
  const info = Reflect.typeOf(boxed);
  const number = info.findField('number');
  const text = info.findField('text');
  if (number == null || text == null || number.getValue(boxed) != 20 ||
      text.getValue(boxed) != 'kept') { return -1; }
  holder.pair.maybe = Empty();
  if (holder.pair.maybe == null || holder.pair.text != 'kept' || pair.maybe != null) { return -2; }
  const erased: dynamic = holder;
  const tail = Reflect.typeOf(erased).findField('tail');
  if (tail == null || tail.getValue(erased) != 22) { return -3; }
  return holder.pair.number + holder.tail;
}
)");
}

TEST(StructTest, NarrowedClassReceiversUseDerivedInlineFieldLayouts) {
  test::ExpectRunAnswer(R"(
struct Position { x: int; y: int; }
class Base { marker: int; }
class Derived extends Base { position: Position; tail: int; }
func update(value: Base): int {
  if (!(value is Derived)) { return -1; }
  const snapshot = value.position;
  value.position = Position(x: 20, y: 21);
  value.tail = 1;
  value.position.y++;
  return snapshot.x == 1 && snapshot.y == 2 && value.tail == 1
      ? value.position.x + value.position.y : -2;
}
func main(): int {
  return update(Derived(marker: 0, position: Position(x: 1, y: 2), tail: 0));
}
)");
}

TEST(StructTest, MutableMethodsUpdateLocalNestedAndArrayPlacesBeforeCallbacksAndThrow) {
  test::ExpectRunAnswer(R"(
class Stop {}
struct Counter {
  value: int;
  advance(observe: () => void): void {
    this.value += 1;
    observe();
    this.value += 1;
  }
  fail(): void { this.value += 3; throw Stop(); }
}
struct Pair { counter: Counter; other: int; }
func main(): int {
  let local = Counter(value: 1);
  local.advance(() => {});
  let pair = Pair(counter: Counter(value: 4), other: 0);
  pair.counter.advance(() => { pair.other = pair.counter.value; });
  const items = [Pair(counter: Counter(value: 10), other: 0)];
  items[0].counter.advance(() => {
    items[0].other = items[0].counter.value;
    for (let i = 0; i < 100; i++) { items.push(Pair(counter: Counter(value: i), other: i)); }
  });
  try { items[0].counter.fail(); } catch (error: dynamic) {}
  if (local.value != 3) { return -100 - local.value; }
  if (pair.counter.value != 6) { return -200 - pair.counter.value; }
  if (pair.other != 5) { return -300 - pair.other; }
  if (items[0].counter.value != 15 || items[0].other != 11) { return -2; }
  return 42;
}
)");
}

TEST(StructTest, MethodAndPropertyReceiversRespectReadonlyCopiesAndBoxIdentity) {
  test::ExpectRunAnswer(R"(
interface CounterApi { add(): int; read(): int; }
struct Counter implements CounterApi {
  value: int;
  add(): int { this.value++; return this.value; }
  read(): int { return this.value; }
  get next(): int { this.value++; return this.value; }
  get number(): int { return this.value; }
  set number(value: int): void { this.value = value; }
}

struct Nested { value: Counter; }
class Holder { const frozen: Counter; value: Counter; }
func make(): Nested { return Nested(value: Counter(value: 10)); }
func main(): int {
  let local = Counter(value: 1);
  local.number = 5;
  if (local.next != 6 || local.value != 6) { return -1; }
  local.number += 1;
  if (local.value != 7) { return -2; }
  const frozen = local;
  if (frozen.add() != 8 || frozen.value != 7 || frozen.next != 8 || frozen.value != 7) { return -3; }
  const holder = Holder(frozen: frozen, value: frozen);
  holder.frozen.add();
  holder.value.number = 20;
  if (holder.frozen.value != 7 || holder.value.add() != 21) { return -4; }
  const bound = local.add;
  const boxed: CounterApi = local;
  if (bound() != 8 || bound() != 9 || boxed.add() != 8 || boxed.read() != 8 || local.value != 7) { return -5; }
  const erased: dynamic = local;
  erased.add();
  if (erased.value != 8 || local.value != 7 || make().value.add() != 11) { return -6; }
  return 42;
}
)");
}

TEST(StructTest, AssignmentsUseSettersWithoutReadingAndKeepNestedLocations) {
  test::ExpectRunAnswer(R"(
let reads = 0;
struct Counter {
  value: int;
  get number(): int { reads++; return this.value; }
  set number(value: int): void { this.value = value; }
}
struct Pair { counter: Counter; other: int; }
const pairs = [Pair(counter: Counter(value: 1), other: 2)];
func change(): int { pairs[0].other = 20; return 21; }
func main(): int {
  pairs[0].counter.number = change();
  if (reads != 0 || pairs[0].other != 20 || pairs[0].counter.value != 21) { return -1; }
  pairs[0].counter.number += 1;
  if (reads != 1 || pairs[0].counter.value != 22) { return -2; }
  return pairs[0].other + pairs[0].counter.value;
}
)");
}

TEST(StructTest, FillCopiesNullableValuesAndKeepsReferencesAlive) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Entry { text: string; number: int?; }
func main(): int {
  const entries: Entry?[] = [Entry(text: 'keep'.toUpperCase(), number: null), null, null, null];
  entries.fill(Entry(text: 'alive'.toUpperCase(), number: 42), 1, 3);
  entries.fill(null, 2, 2);
  for (let i = 0; i < 40; i++) { const garbage = 'temporary:${i}'.toUpperCase(); }
  if (entries[0]?.text != 'KEEP' || entries[1]?.text != 'ALIVE' ||
      entries[1]?.number != 42 || entries[2]?.text != 'ALIVE' || entries[3] != null) {
    return 0;
  }
  entries.fill(null, 2, 4);
  return entries[1]?.number == 42 && entries[2] == null && entries[3] == null ? 42 : 0;
}
)",
                        isolate);
}

TEST(StructTest, IncludesInlineValuesPreservesNullableAndSameValueZeroSemantics) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Empty {}
struct Key { number: double; text: string; optional: int?; }
func main(): int {
  const empty: Empty?[] = [null, Empty()];
  if (!empty.includes(null) || empty.includes(null, 1) || !empty.includes(Empty(), 1) ||
      empty.includes(Empty(), 2) || empty.includes(null, 2)) { return -1; }
  const absent: Empty?[] = [null];
  if (absent.includes(Empty()) || !absent.includes(null)) { return -2; }
  const nan = double.parse('NaN');
  const keys: Key?[] = [null, Key(number: nan, text: 'keep'.toUpperCase(), optional: null),
                       Key(number: -0.0, text: 'zero'.toUpperCase(), optional: 42)];
  if (!keys.includes(Key(number: nan, text: 'KEEP', optional: null)) ||
      keys.includes(Key(number: nan, text: 'KEEP', optional: 42)) ||
      !keys.includes(Key(number: 0.0, text: 'ZERO', optional: 42), -1) ||
      keys.includes(null, 1) || keys.includes(Key(number: nan, text: 'KEEP', optional: null), 2)) {
    return -3;
  }
  const none: Key[] = [];
  return none.includes(Key(number: nan, text: 'KEEP', optional: null)) ? -4 : 42;
}
)",
                        isolate);
}

TEST(StructTest, NullDefaultsSkipOnlyFullyInitializedGeneratedConstructors) {
  test::ExpectRunAnswer(R"(
let initializations = 0;
func next(): int { initializations++; return initializations; }
struct Entry { value: int = next(); text: string? = null; }
struct PrivateEntry {
  value: int = next();
  text: string? = null;
  #stamp: int = next();
  stamp(): int { return this.#stamp; }
}
func main(): int {
  const direct = Entry(value: 40);
  if (initializations != 0 || direct.text != null) { return -1; }
  const defaults = Entry();
  if (initializations != 1 || defaults.value != 1 || defaults.text != null) { return -2; }
  const privateEntry = PrivateEntry(value: 42);
  if (initializations != 2 || privateEntry.stamp() != 2 || privateEntry.text != null) { return -3; }
  return direct.value + defaults.value + initializations - 1;
}
)");
}

TEST(StructTest, MutableMethodsTrackNullablePlacesAndRejectRemovedArrayElements) {
  test::ExpectRunAnswer(R"(
struct Counter {
  value: int;
  advance(observe: () => void): void { this.value++; observe(); this.value++; }
}
struct Pair { counter: Counter?; other: int; }
class Holder { counter: Counter?; }
let receiverCalls = 0;
let indexCalls = 0;
const items: Counter?[] = [Counter(value: 10)];
func receiver(): Counter?[] { receiverCalls++; return items; }
func index(): int { indexCalls++; return 0; }
func main(): int {
  let local: Counter? = Counter(value: 1);
  local?.advance(() => {});
  if (local == null || local.value != 3) { return -1; }
  let pair = Pair(counter: Counter(value: 4), other: 0);
  pair.counter?.advance(() => { pair.other = 9; });
  if (pair.counter?.value != 6 || pair.other != 9) { return -2; }
  const holder = Holder(counter: Counter(value: 7));
  holder.counter?.advance(() => {});
  if (holder.counter?.value != 9) { return -3; }
  receiver()[index()]?.advance(() => { items.push(null); });
  if (receiverCalls != 1 || indexCalls != 1 || items[0]?.value != 12) { return -4; }
  let observed = 0;
  let failed = false;
  try {
    items[0]?.advance(() => { observed = items[0]?.value ?? 0; items.clear(); });
  } catch (error: dynamic) { failed = true; }
  if (!failed || observed != 13 || items.length != 0) { return -5; }
  return 42;
}
)");
}

TEST(StructTest, ErasedCopiesAndMultipleArgumentsSurviveGc) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct NamedValue { text: string; value: int; }
func read(first: dynamic, second: dynamic): int {
  for (let i = 0; i < 40; i++) { const garbage = 'temporary:${i}'.toUpperCase(); }
  if (first.text != 'FIRST' || second.text != 'SECOND') { return -1; }
  first.value = 19;
  return (first.value as int) + (second.value as int);
}
func erase(value: NamedValue): dynamic { return value; }
func main(): int {
  let first = NamedValue(text: 'first'.toUpperCase(), value: 20);
  const second = NamedValue(text: 'second'.toUpperCase(), value: 23);
  const result = read(first, second);
  if (first.value != 20 || result != 42) { return -2; }
  const boxed = erase(first);
  return (boxed.value as int) + 22;
}
)",
                        isolate);
}

TEST(StructTest, NestedEqualityAndCollectionKeysFollowValueSemantics) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Label { text: string; }
struct Data { label: Label?; number: double?; }
struct Key { number: double; text: string?; }
func main(): int {
  let changed = Data(label: Label(text: 'temporary'.toUpperCase()), number: null);
  changed.label = null;
  const missing = Data(label: null, number: null);
  if (changed != missing) { return -1; }
  const erased: dynamic = changed;
  const other: dynamic = missing;
  if (erased != other) { return -2; }
  const values: Data?[] = [missing, null];
  if (!values.includes(changed) || !values.includes(null)) { return -3; }
  const nan = double.parse('NaN');
  const key = Key(number: nan, text: null);
  if (key == key) { return -4; }
  const boxed: dynamic = key;
  if (boxed == boxed) { return -5; }
  const keys = Set<Key>();
  keys.add(key);
  keys.add(Key(number: nan, text: null));
  if (keys.size != 1 || !keys.has(Key(number: nan, text: null))) { return -6; }
  const map = Map<Key, int>();
  map.set(Key(number: -0.0, text: 'key'), 42);
  return map.get(Key(number: 0.0, text: 'key')) ?? -7;
}
)",
                        isolate);
}

TEST(StructTest, TypedCollectionsCopyStructsAcrossErasedAccessAndIteration) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Key { id: int; }
struct Data { number: int; label: string; }
func main(): int {
  const map = Map<Key, Data>();
  const erased: dynamic = map;
  const key: dynamic = Key(id: 1);
  const value: dynamic = Data(number: 42, label: 'value'.toUpperCase());
  erased.set(key, value);
  key.id = 2;
  value.number = 0;
  if (key.id != 2 || value.number != 0) { return -4; }
  if (!map.has(Key(id: 1))) { return -1; }
  const loaded: dynamic = erased.get(Key(id: 1));
  if (loaded.number != 42 || loaded.label != 'VALUE') { return -2; }
  loaded.number = 99;
  for (const item in map.values()) {
    const boxed: dynamic = item;
    boxed.number = 100;
  }
  const result = map.get(Key(id: 1));
  return result == null ? -3 : result.number;
}
)",
                        isolate);
}

TEST(StructTest, TypedEqualityDoesNotRequireBoxAllocations) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(R"(
struct Position { x: double; y: double; z: double; }
struct Body { position: Position?; active: bool; }
func main(): int {
  const first = Body(position: Position(x: 1.0, y: 2.0, z: 3.0), active: true);
  const second = Body(position: Position(x: 1.0, y: 2.0, z: 3.0), active: true);
  const missing = Body(position: null, active: false);
  return first == second && first != missing && missing == missing ? 42 : 0;
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  isolate.heap().SetAllocationCountLimit(0);
  test::ExpectAnswer(**compiled);
}

TEST(StructTest, DynamicNestedFieldsUseLogicalIndicesAndCopyValues) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Position { x: int; y: int; name: string; }
struct Body { position: Position?; active: bool; count: int; }
func main(): int {
  const original = Body(position: Position(x: 10, y: 20, name: 'original'), active: true, count: 1);
  const boxed: dynamic = original;
  const copy: dynamic = boxed.position;
  copy.x = 99;
  if (boxed.position.x != 10 || !(boxed.active as bool) || boxed.count != 1) { return -1; }
  const replacement = Position(x: 40, y: 2, name: 'replaced'.toUpperCase());
  boxed.position = replacement;
  boxed.count = 2;
  for (let i = 0; i < 40; i++) { const garbage = 'temporary:${i}'.toUpperCase(); }
  const restored = boxed as Body;
  const restoredPosition = restored.position;
  if (restoredPosition == null || restoredPosition.name != 'REPLACED' || restored.count != 2) {
    return -2;
  }
  boxed.position = null;
  boxed.active = false;
  const empty = Body(position: null, active: false, count: 2);
  const missing: dynamic = empty;
  const originalPosition = original.position as Position;
  if (boxed != missing || boxed.position != null || originalPosition.x != 10) { return -3; }
  return restoredPosition.x + restoredPosition.y;
}
)",
                        isolate);
}

TEST(StructTest, ArrayStorageCopiesNestedValuesAndSurvivesGrowthAndRuntimeOperations) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Position { x: int; y: int; z: int; }
struct Body { position: Position; active: bool; }
func main(): int {
  const bodies: Body[] = [];
  let original = Body(position: Position(x: 10, y: 20, z: 30), active: true);
  for (let i = 0; i < 40; i++) { bodies.push(original); }
  original.active = false;
  let copy = bodies[0];
  copy.active = false;
  if (!bodies[0].active) { return -1; }
  bodies[1] = copy;
  if (bodies[1].active) { return -2; }
  const slice = bodies.slice(0, 3);
  slice.reverse();
  const last = bodies.pop();
  if (last == null || !last.active) { return -3; }
  return bodies[0].position.x + slice[0].position.y + last.position.z - 18;
}
)",
                        isolate);
}

TEST(StructTest, NestedFieldWritesKeepOriginalLocationsAndEvaluateReceiversOnce) {
  test::ExpectRunAnswer(R"(
struct Position { x: int; y: int; }
struct Body { position: Position; active: bool; }
const items: Body[] = [Body(position: Position(x: 10, y: 20), active: true)];
let receivers = 0;
let indices = 0;
func receiver(): Body[] { receivers++; return items; }
func index(): int { indices++; return 0; }
func change(): int {
  for (let i = 0; i < 12; i++) {
    items.push(Body(position: Position(x: 1, y: 2), active: false));
  }
  items[0].position.y = 32;
  items[0].active = false;
  return 1;
}
func main(): int {
  receiver()[index()].position.x += change();
  let local = items[0];
  local.position.x++;
  if (local.position.x != 12 || items[0].position.x != 11) { return -1; }
  if (receivers != 1 || indices != 1 || items[0].active) { return -2; }
  return items[0].position.x + items[0].position.y - 1;
}
)");
}

TEST(StructTest, CopiesLocalValuesAndPassesLargeNestedValuesThroughTypedCalls) {
  test::ExpectRunAnswer(R"(
struct Position { x: int; y: int; z: int; }
struct Body { position: Position; active: bool; }
func make(): Body { return Body(position: Position(x: 10, y: 20, z: 30), active: true); }
func change(value: Body): Body { value.active = false; return value; }
func main(): int {
  let first = make();
  let second = first;
  second.active = false;
  const third = change(first);
  if (!first.active) { return -1; }
  if (second.active) { return -2; }
  if (third.active) { return -3; }
  return first.position.x + second.position.y + third.position.z - 18;
}
)");
}

TEST(StructTest, GenericFieldsInterfacesAndBoundMethodsKeepIndependentValues) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
interface Reader { read(): int; }
struct Position { x: int; y: int; z: int; }
struct Record<T> implements Reader {
  value: T;
  position: Position;
  read(): int { return this.position.x + this.position.y + this.position.z; }
}
func invoke(read: () => int): int { return read(); }
func use(reader: Reader): int { return reader.read(); }
func main(): int {
  let first = Record<string>(value: 'record'.toUpperCase(), position: Position(x: 10, y: 20, z: 12));
  const bound = first.read;
  const erased: Reader = first;
  const fields = [first];
  first.position.x = 99;
  for (let i = 0; i < 40; i++) { const garbage = 'temporary:${i}'.toUpperCase(); }
  if (fields[0].value != 'RECORD' || use(erased) != 42 || bound() != 42) { return -1; }
  return invoke(bound);
}
)",
                        isolate);
}

TEST(StructTest, ClassFieldsEmbedNestedValuesAndDynamicReadsCopyThem) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Position { x: int; y: int; name: string; }
struct Body { position: Position?; active: bool; }
class Base { first: int; body: Body; }
class Holder extends Base { optional: Body?; last: int; }
func main(): int {
  const body = Body(position: Position(x: 10, y: 20, name: 'kept'.toUpperCase()), active: true);
  const holder = Holder(first: 1, body: body, optional: body, last: 2);
  holder.body.position = Position(x: 20, y: 22, name: 'changed'.toUpperCase());
  const erased: dynamic = holder;
  const snapshot: dynamic = erased.body;
  snapshot.active = false;
  if (!holder.body.active || holder.first != 1 || holder.last != 2) { return -1; }
  erased.optional = null;
  for (let i = 0; i < 40; i++) { const garbage = 'temporary:${i}'.toUpperCase(); }
  const position = holder.body.position;
  const original = body.position;
  if (position == null || original == null || position.name != 'CHANGED' ||
      original.name != 'KEPT' || holder.optional != null) { return -2; }
  erased.body = body;
  if ((holder.body.position as Position).x != 10) { return -3; }
  return position.x + position.y;
}
)",
                        isolate);
}

TEST(StructTest, ClassFieldConstructionAndTypedAccessNeedNoBoxes) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(R"(
struct Position { x: int; y: int; z: int; }
class Holder { before: int; position: Position; after: int; }
func main(): int {
  const holder = Holder(before: 1, position: Position(x: 10, y: 20, z: 30), after: 2);
  holder.position.x += 2;
  return holder.position.x + holder.position.y + holder.position.z - 20;
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  isolate.heap().SetAllocationCountLimit(1);
  test::ExpectAnswer(**compiled);
}

TEST(StructTest, NullableClassFieldsKeepDeclaredStorageAcrossConstructionAndCachedWrites) {
  test::ExpectRunAnswer(R"(
struct Position { x: int; y: int; name: string; }
class Body { position: Position?; count: int; }
func create(index: int): Body {
  return Body(position: Position(x: index, y: 2, name: 'original'), count: index);
}
func replace(body: Body, value: Position?): void { body.position = value; }
func main(): int {
  let sum = 0;
  for (let i = 0; i < 3; i++) {
    const body = create(10 + i);
    const original = body.position as Position;
    if (original.x != 10 + i || original.y != 2 || original.name != 'original') { return -1; }
    replace(body, Position(x: 40, y: 2, name: 'replacement'));
    if (body.position?.x != 40 || body.position?.name != 'replacement') { return -2; }
    replace(body, null);
    if (body.position != null || body.count != 10 + i || original.x != 10 + i) { return -3; }
    body.position = Position(x: 12, y: 2, name: 'direct');
    sum += body.position?.x ?? 0;
    sum += body.position?.y ?? 0;
  }
  return sum;
}
)");
}

TEST(StructTest, ClosuresCopyConstantCapturesAndShareMutableStructBindings) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Position { x: int; y: int; text: string; }
func main(): int {
  const fixed = Position(x: 10, y: 20, text: 'fixed'.toUpperCase());
  const tail = 2;
  let changed = Position(x: 1, y: 2, text: 'mutable'.toUpperCase());
  const read = (): int => fixed.text == 'FIXED' ? fixed.x + fixed.y + tail : 0;
  const increment = (): void => { changed.x += 10; };
  increment();
  for (let i = 0; i < 40; i++) { const garbage = 'temporary:${i}'.toUpperCase(); }
  return changed.text == 'MUTABLE' ? read() + changed.x - 1 : 0;
}
)",
                        isolate);
}

TEST(StructTest, EmbeddedEmptyAndOptionalScalarFieldsHaveDistinctStorage) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Empty {}
struct Values { flag: bool; maybe: int?; number: double?; }
class Holder { empty: Empty; optional: Empty?; values: Values?; tail: int; }
func main(): int {
  const holder = Holder(empty: Empty(), optional: null,
                        values: Values(flag: true, maybe: 40, number: 2.0), tail: 42);
  const values = holder.values;
  if (values == null || !values.flag || values.maybe != 40 || values.number != 2.0) { return -1; }
  const erased: dynamic = holder;
  if (erased.empty == null || erased.optional != null) { return -2; }
  erased.optional = Empty();
  erased.values = null;
  return holder.optional != null && holder.values == null ? holder.tail : 0;
}
)",
                        isolate);
}

TEST(StructTest, ConstructorsReturnInitializedValuesAcrossDefaultsEarlyReturnAndFinally) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
struct Position { x: int; y: int; z: int; }
struct Body {
  position: Position = Position(x: 10, y: 20, z: 0);
  text: string = 'created'.toUpperCase();
  constructor(extra: int = 2) {
    try {
      this.position.z = extra;
      if (extra == 2) { return; }
      this.position.x = 0;
    } finally {
      this.position.y += 10;
    }
  }
}
struct Defaults { position: Position = Position(x: 10, y: 20, z: 12); }
func main(): int {
  const first = Body();
  const make: () => Body = Body;
  const second = make();
  const explicit = Body(extra: 1);
  const defaults = Defaults();
  for (let i = 0; i < 40; i++) { const garbage = 'temporary:${i}'.toUpperCase(); }
  return first.text == 'CREATED' && second.position.y == 30 && explicit.position.x == 0 &&
         defaults.position.z == 12 ? first.position.x + first.position.y + first.position.z : 0;
}
)",
                        isolate);
}

TEST_P(ClassDispatchTest, SelectsActualReceiverImplementationsUnderGcPressure) {
  test::ExpectRunAnswer(GetParam());
}

INSTANTIATE_TEST_SUITE_P(CallsPropertiesAndMethodValues,
                         ClassDispatchTest,
                         testing::Values(
                             R"(
interface Reader { read(): int; }
class Base implements Reader {
  read(): int { return 1; }
  twice(): int { return this.read() + this.read(); }
}
class Child extends Base { override read(): int { return 14; } }
func use(value: Base, reader: Reader): int { return value.twice() + reader.read(); }
func main(): int { const child = Child(); return use(child, child); }
)",
                             R"(
class Root {
  read(): int { return 1; }
  twice(): int { return this.read() + this.read(); }
}
class Middle extends Root { override read(): int { return 20; } }
class Leaf extends Middle {
  override read(): int { const parent: () => int = super.read; return parent() + 1; }
}
func use(value: Root): int { return value.twice(); }
func main(): int { return use(Leaf()); }
)",
                             R"(
let calls = 0;
class Base { read(): int { return 0; } }
class Child extends Base { override read(): int { return 42; } }
func make(): Base { calls++; return Child(); }
func main(): int {
  const bound: () => int = make().read;
  for (let index = 0; index < 50; index++) { const garbage = 'item:${index}'; }
  return calls == 1 ? bound() : 0;
}
)",
                             R"(
let calls = 0;
func argument(): int { calls++; return 21; }
interface Reader { read(value: int): int; }
class Base implements Reader { read(_value: int): int { return 0; } }
class Child extends Base { override read(value: int): int { return value; } }
func use(value: Base?, reader: Reader?): int {
  const first = value?.read(argument()) ?? 0;
  const second = reader?.read(argument()) ?? 0;
  return first + second;
}
func main(): int {
  if (use(null, null) != 0 || calls != 0) { return 0; }
  const child = Child();
  const result = use(child, child);
  return calls == 2 ? result : 0;
}
)",
                             R"(
class Base {
  stored: int = 0;
  get value(): int { return this.stored; }
  set value(next: int): void { this.stored = next; }
}
class Child extends Base {
  override get value(): int { return this.stored + 2; }
  override set value(next: int): void { this.stored = next * 2; }
}
func use(value: Base): int { value.value += 18; return value.value; }
func main(): int { return use(Child()); }
)",
                             R"(
class Base {
  stored: int = 0;
  get value(): int { return this.stored + 1; }
  set value(next: int): void { this.stored = next; }
}
class WithGetter extends Base { override get value(): int { return this.stored + 2; } }
class WithSetter extends WithGetter {
  override set value(next: int): void { this.stored = next * 2; }
}
func use(value: Base): int { value.value = 20; return value.value; }
func main(): int {
  const value = WithSetter();
  value.value = 1;
  if (value.value != 4) { return 0; }
  return use(value);
}
)",
                             R"(
class Base {
  stored: int = 0;
  get value(): int { return this.stored; }
  set value(next: int): void { this.stored = next; }
}
class Child extends Base {
  override get value(): int { return super.value + 2; }
  override set value(next: int): void { super.value = next * 2; }
}
func use(value: Base): int {
  value.value++;
  return value.value == 8 ? 42 : 0;
}
func main(): int { return use(Child()); }
)",
                             R"(
class Base { read<T>(_value: T): int { return 1; } }
class Child extends Base { override read<U>(_value: U): int { return 21; } }
func use(value: Base): int {
  const bound: (int) => int = value.read;
  return value.read<int>(0) + bound(1);
}
func main(): int { return use(Child()); }
)",
                             R"(
class Base<T> { read<U>(_value: U): int { return 1; } }
class Child<T> extends Base<T> {
  override read<V>(_value: V): int { return 21; }
}
func use<T extends Base<int>>(value: T): int {
  const bound: (string) => int = value.read;
  return value.read<int>(0) + bound('value');
}
func call(value: Base<int>): int { return use<Base<int>>(value); }
func main(): int { return call(Child<int>()); }
)",
                             R"(
class Base { read(): int { return 1; } }
class Child extends Base { override read(): int { return 14; } }
func use(value: Base): int {
  const before: () => int = value.read;
  if (value is Child) { return before() + value.read() + (value as Base).read(); }
  return 0;
}
func main(): int { return use(Child()); }
)",
                             R"(
interface Writer { write(): void; }
class Base implements Writer {
  stored: int = 0;
  write(): void { this.stored = 1; }
}
class Child extends Base { override write(): void { this.stored = 42; } }
func use(value: Base, writer: Writer): int {
  value.write();
  if (value.stored != 42) { return 0; }
  value.stored = 0;
  const bound: () => void = writer.write;
  bound();
  return value.stored;
}
func main(): int { const value = Child(); return use(value, value); }
)",
                             R"(
class Base { read(): int { return 0; } }
class Child extends Base { override read(): int { return 21; } }
func main(): int {
  const value: dynamic = Child();
  const bound = value.read;
  return (value.read() + bound()) as int;
}
)",
                             R"(
class Base { toJson(): int[] { return [0]; } }
class Child extends Base { override toJson(): int[] { return [40, 2]; } }
func encode(value: Base): string { return JSON.stringify(value); }
func main(): int { return encode(Child()) == '[40,2]' ? 42 : 0; }
)",
                             R"(
let observed = 0;
class Base {
  constructor { observed = this.read(); }
  read(): int { return 0; }
}
class Child extends Base {
  value: int;
  constructor(value: int) { this.value = value; super(); }
  override read(): int { return this.value; }
}
func main(): int { const value = Child(42); return observed; }
)",
                             R"(
enum Choice { first, second }
class Base { read(_value: Choice): Choice { return Choice.first; } }
class Child extends Base { override read(value: Choice): Choice { return value; } }
func use(value: Base): int {
  const bound: (Choice) => Choice = value.read;
  return bound(Choice.second) == Choice.second ? 42 : 0;
}
func main(): int { return use(Child()); }
)"));

TEST(ClassDispatchAsyncTest, AsyncMethodsAndBoundCallbacksUseTheActualReceiver) {
  test::ExpectFutureAnswer(R"(
class Base { async read(): Future<int> { await 0; return 0; } }
class Child extends Base { override async read(): Future<int> { await 0; return 21; } }
async func use(value: Base): Future<int> {
  const bound: () => Future<int> = value.read;
  return (await value.read()) + (await bound());
}
async func main(): Future<int> { return await use(Child()); }
)",
                           test::kExpectedAnswer);
}

TEST(ClassDispatchTest, DynamicPropertiesRespectEachAccessorVisibilityAndInheritedOwner) {
  test::ExpectRunAnswer(R"(
class Base {
  stored: int = 21;

  set #value(next: int): void { this.stored = next; }

  get value(): int { return this.stored; }
}
class Child extends Base { override get value(): int { return super.value; } }
class WriteOnly {
  stored: int = 0;

  get #value(): int { return this.stored; }

  set value(next: int): void { this.stored = next; }
}
func main(): int {
  const reader: dynamic = Child();
  const writer: dynamic = WriteOnly();
  writer.value = 21;
  let caught = 0;
  try { reader.value = 0; } catch (error: Error) { if (error.code == 'TypeError') { caught++; } }
  try { const value = writer.value; }
  catch (error: Error) { if (error.code == 'TypeError') { caught++; } }
  return caught == 2 ? (reader.value + writer.stored) as int : 0;
}
)");
}

TEST(ClassDispatchTest, StaticWritesCheckSetterVisibilityForEveryMutation) {
  for (const auto* mutation : {"value.amount = 1;", "value.amount += 1;", "value.amount++;"}) {
    test::ExpectRejectedCheck(std::string(R"(
class Counter {
  stored: int = 0;
  get amount(): int { return this.stored; }

  set #amount(next: int): void { this.stored = next; }
}
func main(): void { const value = Counter();
)") + mutation + " }");
  }
}
class ClassConstructionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ClassConstructionTest, InitializesFieldsAndExecutesConstructorUnderGcPressure) {
  test::ExpectRunAnswer(GetParam());
}

INSTANTIATE_TEST_SUITE_P(GeneratedAndExplicitConstructors,
                         ClassConstructionTest,
                         testing::Values(
                             R"(
class Counter {
  value: int;
  step: int = 1;

  #count: int = 0;

  constructor { this.#count = this.#count + 1; }
  next(): int { this.value = this.value + this.step; return this.value + this.#count; }
}
func main(): int { const c = Counter(value: 40); return c.next(); }
)",
                             R"(
func defaultStep(): int { return 2; }
class Counter {
  value: int;
  step: int = defaultStep();
  next(): int { return this.value + this.step; }
}
func main(): int {
  const defaultStep = (): int => 99;
  return Counter(value: 40).next();
}
)",
                             R"(
class Pair {
  value: int;

  #label: string = "a" + "b";

  constructor(value: int, positive: bool) {
    if (positive) { this.value = value; } else { this.value = -value; }
  }
  read(): int { return this.value + this.#label.length; }
}
func main(): int { return Pair(40, true).read(); }
)",
                             R"(
class Box<T> {

  #value: T;

  constructor(value: T) { this.#value = value; }
  get(): T { return this.#value; }
}
type IntegerBox = Box<int>;
func main(): int { return IntegerBox(value: 42).get(); }
)",
                             R"(
class Box {
  value: int;
  constructor() { while (true) { this.value = 42; break; } }
}
func main(): int { return Box().value; }
)",
                             R"(
class Box {
  value: int;
  constructor(value: int) {
    switch (value) {
      case 0: this.value = 0; break;
      default: this.value = value; break;
    }
  }
}
func main(): int { return Box(42).value; }
)",
                             R"(
class Counter {

  static #value: int = 40;

  static next(): int { Counter.#value = Counter.#value + 1; return Counter.#value; }
}
func main(): int { const f: () => int = Counter.next; f(); return f(); }
)",
                             R"(
class Callback {
  static operation: (int) => int = (value: int): int => value + 2;
}
func main(): int { return Callback.operation(40); }
)",
                             R"(
class Value {
  value: int;
  constructor() { if (true) { this.value = 42; } }
}
func main(): int { return Value().value; }
)",
                             R"(
class Box {
  value: int;
  text: string = 'default';

  #adjustment: int = 2;

  constructor(initial: int) { this.value = initial + this.#adjustment; }
}
func main(): int { return Box(initial: 40).value; }
)",
                             R"(
class Box { text: string?; optional: string? = null; }
func main(): int {
  const box = Box(text: null);
  return box.text == null && box.optional == null ? 42 : 0;
}
)",
                             R"(
class Box { value: int; param: int; }
func main(): int { return Box(value: 40, param: 2).value + 2; }
)"));

INSTANTIATE_TEST_SUITE_P(Inheritance,
                         ClassConstructionTest,
                         testing::Values(
                             R"(
class Counter { value: int = 40; }
class BonusCounter extends Counter {
  bonus: int;
  total(): int { return this.value + this.bonus; }
}
func main(): int { return BonusCounter(bonus: 2).total(); }
)",
                             R"(
class Counter {
  value: int = 40;
  label: string = "base";
  constructor { this.value += 1; }
}
class BonusCounter extends Counter {
  bonus: int;
  constructor { super(); }
}
func main(): int {
  const counter = BonusCounter(bonus: 1);
  if (counter.label == "base") return counter.value + counter.bonus;
  return 0;
}
)",
                             R"(
class Counter { value: int; }
class BonusCounter extends Counter {
  bonus: int;
  constructor { super(value: this.bonus * 20); }
}
func main(): int {
  const counter = BonusCounter(bonus: 2);
  return counter.value + counter.bonus;
}
)",
                             R"(
class Counter {
  value: int = 39;
  adjustment: int = 1;
  constructor { this.value += this.adjustment; }
}
class BonusCounter extends Counter {
  bonus: int;
  constructor { super(); }
}
func main(): int {
  const counter = BonusCounter(bonus: 2);
  return counter.value + counter.bonus;
}
)",
                             R"(
class Counter {
  value: int;
  adjustment: int = 1;
  constructor(value: int) { this.value = value + this.adjustment; }
}
class BonusCounter extends Counter {
  bonus: int;
  constructor { super(this.bonus * 20 - 1); }
}
func main(): int {
  const counter = BonusCounter(bonus: 2);
  return counter.value + counter.bonus;
}
)",
                             R"(
class Counter {
  value: int;
  constructor { this.value += 1; }
}
class BonusCounter extends Counter {
  bonus: int;
  constructor(value: int, bonus: int) {
    this.bonus = bonus;
    super(value: value);
  }
}
func main(): int {
  const counter = BonusCounter(39, 2);
  return counter.value + counter.bonus;
}
)",
                             R"(
class Root {
  value: int;
  constructor { this.value += 1; }
}
class Middle extends Root {
  middle: int;
  constructor { super(value: this.middle * 10 - 1); }
}
class Leaf extends Middle {
  bonus: int;
  constructor { super(middle: this.bonus * 2); }
}
func main(): int {
  const counter = Leaf(bonus: 2);
  return counter.value + counter.bonus;
}
)",
                             R"(
class Counter {
  value: int;
  label: string = "parent" + " initialized";
}
class BonusCounter extends Counter {
  bonus: int;
  constructor { super(value: this.bonus * 20); }
}
func main(): int {
  const counter = BonusCounter(bonus: 2);
  if (counter.label == "parent initialized") return counter.value + counter.bonus;
  return 0;
}
)"));

INSTANTIATE_TEST_SUITE_P(InheritedSignatures,
                         ClassConstructionTest,
                         testing::Values(
                             R"(
class Base<P> {
  props: P;
  constructor { if (this.ready() != 2) throw Error(message: 'uninitialized child'); }
  ready(): int { return 0; }
}
class Child extends Base<int> {

  #adjustment: int = 2;

  override ready(): int { return this.#adjustment; }
}
func main(): int { return Child(props: 40).props + 2; }
)",
                             R"(
class Root<T> { value: T; }
class Middle<U> extends Root<U[]> {}
class Leaf<V> extends Middle<V> {}
type Numbers<T> = Leaf<T>;
func main(): int { return Numbers(value: [42]).value[0]; }
)",
                             R"(
let calls = 0;
func initial(): int { calls++; return 40; }
class Root {
  value: int;
  constructor(value: int = initial(), answer: int = value + 2) { this.value = answer; }
}
class Middle extends Root {}
class Leaf extends Middle {}
func main(): int {
  const first = Leaf();
  const second = Leaf(answer: 42, value: 1);
  const third = Leaf(value: 40);
  return calls == 1 && first.value == 42 && second.value == 42 ? third.value : 0;
}
)",
                             R"(
let order = '';
func mark(label: string): int { order += label; return 42; }
class Root {
  value: int = mark('base');
  constructor(value: int = mark('default')) { this.value = value; order += 'body'; }
}
class Leaf extends Root {  #state: int = mark('child'); }
func main(): int {
  const value = Leaf();
  return order == 'childbasedefaultbody' ? value.value : 0;
}
)",
                             R"(
// 生成字段构造的默认值与显式参数默认值走不同路径，二者分别验证单次求值。
let calls = 0;
func initial(): int { calls++; return 40; }
class Root { value: int = initial(); bonus: int = 2; }
class Middle extends Root {}
class Leaf extends Middle {}
func main(): int {
  const supplied = Leaf(value: 41, bonus: 1);
  const omitted = Leaf();
  return calls == 1 && supplied.value + supplied.bonus == 42 ? omitted.value + omitted.bonus : 0;
}
)",
                             R"(
class Base<T> {
  value: T;
  constructor(initial: T) { this.value = initial; }
}
class Child<U> extends Base<U> {}
func main(): int { return Child(initial: 42).value; }
)",
                             R"(
class Root {
  value: string?;
  constructor(value: string? = 'default') { this.value = value; }
}
class Leaf extends Root {}
func main(): int { return Leaf().value == 'default' && Leaf(value: null).value == null ? 42 : 0; }
)"));

INSTANTIATE_TEST_SUITE_P(UnifiedConstructors,
                         ClassConstructionTest,
                         testing::Values(
                             R"(
let calls = 0;
func initial(): string? { calls++; return 'default'; }
class Base { value: string? = initial(); }
class Child extends Base { bonus: int = 2; constructor(super.value, this.bonus); }
func main(): int {
  const a = Child();
  const b = Child(null);
  return calls == 1 && a.value == 'default' && b.value == null && b.bonus == 2 ? 42 : 0;
}
)",
                             R"(
let calls = 0;
func initial(): int { calls++; return 40; }
class Base { value: int; constructor(first: int = initial(), second: int = first + 2) { this.value = second; } }
class Child extends Base { constructor(super.second, super.first); }
func main(): int {
  const a = Child();
  const b = Child(first: 1, second: 42);
  return calls == 1 && a.value == 42 ? b.value : 0;
}
)",
                             R"(
class Base { value: int; }
class Child extends Base {

  #bonus: int = 2;

  constructor(super.value, this.#bonus) { this.#bonus += 0; }
  total(): int { return this.value + this.#bonus; }
}
func main(): int { return Child(40).total(); }
)",
                             R"(
class Base { value: int; enabled: bool = true; }
class Child extends Base { bonus: int; }
func main(): int {
  const first = Child(value: 40, bonus: 2);
  const second = Child(value: 1, enabled: false, bonus: 1);
  return first.enabled && !second.enabled ? first.value + first.bonus : 0;
}
)",
                             R"(
class Base { value: int; }
class Child extends Base { bonus: int; constructor { this.bonus++; } }
class Leaf extends Child { extra: int; constructor { this.extra++; } }
func main(): int { const v = Leaf(value: 38, bonus: 1, extra: 1); return v.value + v.bonus + v.extra; }
)",
                             R"(
let calls = 0;
func initial(): int { calls++; return 40; }
class Base { value: int; constructor(initial: int = initial()) { this.value = initial; } }
class Child extends Base { bonus: int = 2; constructor { this.bonus += 0; } }
class Leaf extends Child {}
func main(): int {
  const first = Leaf();
  const second = Leaf(initial: 41, bonus: 1);
  return calls == 1 && first.value + first.bonus == 42 ? second.value + second.bonus : 0;
}
)",
                             R"(
class Base { value: int; }
class Child extends Base { bonus: int; constructor(super.value, this.bonus); }
func main(): int { const v = Child(40, 2); return v.value + v.bonus; }
)",
                             R"(
let calls = 0;
func initial(): int { calls++; return 40; }
class Base { value: int = initial(); }
class Child extends Base { bonus: int = 2; constructor(super.value, this.bonus); }
func main(): int {
  const first = Child();
  const second = Child(bonus: 1, value: 41);
  return calls == 1 && first.value + first.bonus == 42 ? second.value + second.bonus : 0;
}
)",
                             R"(
class Base<T> { value: T; constructor(initial: T) { this.value = initial; } }
class Child<U> extends Base<U> { bonus: int; constructor(super.initial, this.bonus); }
class Leaf<V> extends Child<V> { extra: int = 0; }
func main(): int { const v = Leaf(initial: 40, bonus: 2); return v.value + v.bonus + v.extra; }
)",
                             R"(
class Empty { constructor(); }
class Child extends Empty { value: int; constructor(this.value); }
class Ordinary { value: int = 42; constructor(this.value); }
func main(): int { return Child(Ordinary().value).value; }
)",
                             R"(
class Base { value: int; constructor(value: int) { this.value = value; } }
class Child extends Base {
  bonus: int;
  constructor(value: int, this.bonus) { super(value: value * 2); }
}
func main(): int { const v = Child(20, 2); return v.value + v.bonus; }
)",
                             R"(
class Base { value: int; constructor { if (this.ready() != 2) throw Error(message: 'not ready'); } ready(): int { return 0; } }
class Child extends Base { bonus: int; constructor(super.value, this.bonus); override ready(): int { return this.bonus; } }
func main(): int { return Child(40, 2).value + 2; }
)"));

struct ClassError final {
  std::string_view source;
  std::string_view message;
};

class ClassContractErrorTest : public testing::TestWithParam<ClassError> {};

TEST_P(ClassContractErrorTest, DiagnosesTheViolatedContract) {
  runtime::Isolate isolate;
  const auto& parameter = GetParam();
  const auto compiled = test::CompileSource(parameter.source, isolate);
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [&](const auto& diagnostic) -> bool {
    return diagnostic.message.contains(parameter.message);
  }));
}

INSTANTIATE_TEST_SUITE_P(
    ConstructionAndVisibility,
    ClassContractErrorTest,
    testing::Values(
        ClassError{"class A extends B {} class B extends A {}", "class inheritance cycle"},
        ClassError{"class A { constructor(value: int) {} } class B extends A { value: int; }",
                   "duplicate constructor parameter 'value'"},
        ClassError{"class A { constructor(super.value); }", "unknown super constructor parameter"},
        ClassError{"class A { static value: int = 1; constructor(this.value); }",
                   "own instance field"},
        ClassError{"class A { value: int; } class B extends A { constructor(this.value); }",
                   "own instance field"},
        ClassError{"class A { value: int; } class B extends A { constructor(super.value) { "
                   "super(value: value); } }",
                   "cannot be combined"},
        ClassError{"class A {} class B extends A { value: int; "
                   "constructor() { super(); this.value = 42; } }",
                   "initialized before super"},
        ClassError{"class A {} class B extends A { value: int; "
                   "constructor() { this.value = 42; } }",
                   "initialized before implicit super"},
        ClassError{"class A { x: int; constructor(initial: int) { this.x = initial; } } "
                   "func main(): A { return A(x: 42); }",
                   "unknown named argument 'x'"},
        ClassError{"class A { value: int; } "
                   "class B extends A { bonus: int; constructor { super(); } }",
                   "missing argument 'value'"},
        ClassError{"class A { value: int; } "
                   "class B extends A { bonus: int; constructor { super(value: 40); } } "
                   "func main(): B { return B(value: 40, bonus: 2); }",
                   "unknown named argument 'value'"},
        ClassError{"class A { value: int; } class B extends A { bonus: int; "
                   "constructor(bonus: int) { this.bonus = bonus; } }",
                   "must call super"},
        ClassError{"class A { value: int; } class B extends A { constructor() {} }",
                   "must call super"},
        ClassError{"class A {  #constructor() {} } class B extends A {}",
                   "cannot inherit a private constructor"},
        ClassError{"class A { value: int; } class B extends A {} "
                   "func main(): B { return B(42); }",
                   "must be named"},
        ClassError{"class A { value: int; } class B extends A {} "
                   "func main(): B { return B(); }",
                   "missing argument 'value'"},
        ClassError{"class A {} class B extends A { constructor { super(); super(); } }",
                   "exactly once"},
        ClassError{"class A { value: int; } "
                   "class B extends A { constructor { super(40); } }",
                   "must be named"},
        ClassError{"class A {} class B extends A { constructor { super(); } "
                   "reset(): void { super(); } }",
                   "derived constructor context"},
        ClassError{"class A {} class B extends A { constructor { "
                   "const init = (): void => { super(); }; init(); } }",
                   "derived constructor context"},
        ClassError{"class A { value: int; } "
                   "class B extends A { constructor { super(value: this.value); } }",
                   "read before initialization"},
        ClassError{"class A { value: int = 1; } class B extends A { constructor(flag: bool) { "
                   "if (flag) { super(); } } }",
                   "must call super"},
        ClassError{"class A { value: int = 1; } class B extends A { constructor(flag: bool) { "
                   "if (flag) { super(); } super(); } }",
                   "exactly once"},
        ClassError{"class A { x: int; } func main(): A { return A(1); }", "must be named"},
        ClassError{"class A { x: int; } func main(): A { return A(); }", "missing argument 'x'"},
        ClassError{"class A {  #x: int; }", "requires an initializer"},
        ClassError{"class A {  #x: int = 0; } func main(): A { return A(x: 42); }",
                   "unknown named argument 'x'"},
        ClassError{"class A { x: int; constructor() {} }", "must be initialized"},
        ClassError{"class A { x: int; constructor(flag: bool) { if (flag) this.x = 1; } }",
                   "must be initialized"},
        ClassError{"class A { x: int; constructor() { this.x = this.x + 1; } }",
                   "read before initialization"},
        ClassError{"class A { x: int; constructor() { this.read(); this.x = 1; } read(): int { "
                   "return this.x; } }",
                   "must be initialized"},
        ClassError{"class A { x: int; constructor() { const f = (): int => this.x; this.x = 1; } }",
                   "must be initialized"},
        ClassError{"class A { x: int; constructor() { return; } }", "must be initialized"},
        ClassError{"class A { x: int = this.x; }", "unknown name"},
        ClassError{"class A {  #x: int = 1; } func main(): int { return A().#x; }",
                   "private member"},
        ClassError{"class A {  #get(): int { return 1; } } func main(): int { return A().#get(); }",
                   "private member"},
        ClassError{"class A {  #constructor() {} } func main(): A { return A(); }",
                   "private member"},
        ClassError{"class A { static x: int = 0; } func main(): A { return A(x: 1); }",
                   "unknown named argument 'x'"},
        ClassError{"class A { static read(): int { return this.x; } }", "unknown name"},
        ClassError{
            "class A { static read(): int { return 1; } } func main(): int { return A().read(); }",
            "through its class"},
        ClassError{"class A { read(): int { return 1; } } func main(): int { return A.read(); }",
                   "static member"},
        ClassError{"class A {  static #x: int = 1; } func main(): int { return A.#x; }",
                   "private member"},
        ClassError{
            "class A { f(): int { return 1; } } func main(): void { A().f = (): int => 42; }",
            "immutable"},
        ClassError{
            "class A { static f(): int { return 1; } } func main(): void { A.f = (): int => 42; }",
            "immutable"},
        ClassError{"enum E { a,b } func main(): void { E.a = E.b; }", "immutable"}));

TEST(StaticFieldTest, RetainsRootedStateAcrossExecutionsAndCollection) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileSource(R"(
class State {
  static value: int = 40;
  static text: string = "a" + "b";
}
func main(): int {
  State.value = State.value + 1;
  if (State.text == "ab") { return State.value; }
  return 0;
}
)",
                                            isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto results = test::ExecuteRepeated(*compiled.instance);
  ASSERT_TRUE(results) << results.error();
  EXPECT_EQ(results->front(), 41);
  EXPECT_EQ(results->back(), 42);
}

TEST(StaticFieldTest, DiagnosesInitializationCyclesAndRetriesAfterFailure) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(R"(
class Cycle { static value: int = Cycle.other; static other: int = Cycle.value; }
func main(): int { return Cycle.value; }
)",
                                            isolate);
  ASSERT_TRUE(compiled.Succeeded());
  EXPECT_EQ(test::Execute(*compiled.instance).status, ExecutionStatus::CyclicInitialization);
  EXPECT_EQ(test::Execute(*compiled.instance).status, ExecutionStatus::CyclicInitialization);
}

TEST(ClassInitializationTest, EvaluatesExplicitArgumentsDefaultsAndBodyInOrder) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
class Trace {
  static value: int = 0;
  static mark(value: int): int { Trace.value = Trace.value * 10 + value; return value; }
}
class Target {
  first: int = Trace.mark(9);
  second: int = Trace.mark(8);

  #tail: int = Trace.mark(3);

  constructor { Trace.mark(4); }
}
func main(): int {
  const target = Target(second: Trace.mark(2), first: Trace.mark(1));
  return Trace.value;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 2134);
}

TEST(ClassInitializationTest, DefaultsRunOnlyWhenOmittedAndCreateIndependentCollections) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
class Trace {
  static calls: int = 0;
  static next(): int { Trace.calls++; return Trace.calls; }
}
class Box { value: int = Trace.next(); values: int[] = []; }
func main(): int {
  const supplied = Box(value: 40);
  if (Trace.calls != 0) { return 0; }
  const first = Box();
  const second = Box();
  first.values.push(42);
  return supplied.value == 40 && Trace.calls == 2 && first.value == 1 && second.value == 2 &&
         first.values[0] == 42 && second.values.length == 0 && supplied.values.length == 0 ? 42 : 0;
}
)",
                        isolate);
}

TEST(ClassInitializationTest, DefaultsUseDeclarationScopeBeforeConstructorParameters) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
let seed = 40;
class Generated { value: int = seed; }
class Explicit { value: int = seed; constructor(seed: int) { this.value += seed; } }
func main(): int {
  let seed = 999;
  return Generated().value + Explicit(seed: 2).value - 40;
}
)",
                        isolate);
}

TEST(ClassMutationTest, PropertyUpdatesEvaluateReceiverAndAccessorsOnce) {
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(R"(
let reads = 0;
let writes = 0;
let receivers = 0;
class Counter {

  #stored: int = 39;

  get value(): int { reads++; return this.#stored; }
  set value(next: int): void { writes++; this.#stored = next; }
}
let counter = Counter();
func target(): Counter { receivers++; return counter; }
func main(): int {
  const previous = target().value++;
  const current = ++target().value;
  target().value += 1;
  return previous == 39 && current == 41 && counter.value == 42 &&
         reads == 4 && writes == 3 && receivers == 3 ? 42 : 0;
}
)",
                        isolate);
}

TEST(ClassInitializationTest, OptionalFieldsCrossMaskWordsWithoutAFullFieldArgumentWindow) {
  std::string source =
      "class Trace { static calls: int = 0; static next(): int { Trace.calls++; return 0; } } "
      "class Wide {";
  constexpr int kFields = 130;
  for (int index = 0; index < kFields; ++index) {
    source += "value" + std::to_string(index) + ": int = Trace.next();";
  }
  source +=
      "} func main(): int { const box = Wide(value63: 10, value64: 10, value127: 10, "
      "value128: 12); return Trace.calls == 126 ? "
      "box.value63 + box.value64 + box.value127 + box.value128 : 0; }";
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(source, isolate);
}

TEST(ClassInitializationTest, MergedInheritedFieldDefaultsCrossMaskWords) {
  std::string source =
      "class Trace { static calls: int = 0; static next(): int { Trace.calls++; return 0; } } "
      "class Base {";
  constexpr int kFields = 65;
  for (int index = 0; index < kFields; ++index) {
    source += "base" + std::to_string(index) + ": int = Trace.next();";
  }
  source += "} class Child extends Base {";
  for (int index = 0; index < kFields; ++index) {
    source += "child" + std::to_string(index) + ": int = Trace.next();";
  }
  source +=
      "constructor {} } func main(): int { "
      "const v = Child(base63: 10, base64: 10, child62: 10, child63: 12); "
      "return Trace.calls == 126 ? v.base63 + v.base64 + v.child62 + v.child63 : 0; }";
  test::ExpectRunAnswer(source);
}

TEST(ClassInitializationTest, ForwardingParametersBindTargetsWithoutShadowingGlobals) {
  test::ExpectRunAnswer(R"(
const bonus = 2;
class Base { value: int; }
class Child extends Base {
  bonus: int;
  constructor(super.value, this.bonus) { this.bonus += bonus; }
}
func main(): int { const v = Child(36, 4); return v.value + v.bonus; }
)");
}

}  // namespace
}  // namespace vela::compiler
