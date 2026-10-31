#include <algorithm>
#include <string_view>

#include "compiler/frontend/diagnostic.h"
#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/program_test_support.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/rooted_value.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class JsonProgramTest : public testing::TestWithParam<std::string_view> {};

class DynamicDiagnosticTest : public testing::TestWithParam<std::string_view> {};

TEST_P(DynamicDiagnosticTest, RejectsInvalidInferenceAndStaticContainerConversions) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(GetParam(), isolate);
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_FALSE(compiled.diagnostics.empty());
}

INSTANTIATE_TEST_SUITE_P(
    StaticBoundaries,
    DynamicDiagnosticTest,
    testing::Values(
        "func main(): void { const value: dynamic? = null; }",
        "class dynamic {} func main(): void {}",
        "func empty(): void {} func main(): void { const value: dynamic = empty(); }",
        "func main(): void { const values = [1, true]; }",
        "func main(): void { const values: int[] = [1] as dynamic[]; }",
        "func main(): void { const values: int[] = [1]; const erased: dynamic[] = values; }",
        "func main(): void { const values: Map<string, int> = Map<string, dynamic>(); }",
        "func main(): void { const source: Future<dynamic> = Future.value(1 as dynamic); "
        "const narrowed: Future<int> = source; }",
        "func main(): void { const value: dynamic = missing; }",
        "func main(): void { const value: dynamic = [1]; value.map<int>((item: int): int => item); "
        "}",
        "func main(): void { const callback: dynamic = (): int => 42; callback(value => value); "
        "}"));

class DynamicNominalIdentityTest : public testing::TestWithParam<std::string_view> {};

TEST_P(DynamicNominalIdentityTest, IndependentCompilationsRetainNominalSignatureIdentity) {
  runtime::Isolate isolate(1);
  const auto first = CompileSource(GetParam(), isolate);
  ASSERT_TRUE(first.Succeeded());
  const auto value = vm::VirtualMachine(isolate).Execute(first.program.FindFunction("main"));
  ASSERT_TRUE(value.Succeeded()) << value.message;
  const runtime::RootedValue rooted(isolate, value.value);
  const auto second = CompileSource(GetParam(), isolate);
  ASSERT_TRUE(second.Succeeded());
  const auto original_types = first.program.functions().front().layouts->types();
  const auto foreign_types = second.program.functions().front().layouts->types();
  EXPECT_TRUE(std::ranges::any_of(original_types, [](const auto& type) { return type.callable; }));
  EXPECT_TRUE(std::ranges::all_of(original_types, [&rooted](const auto& type) {
    return !type.callable || runtime::Heap::IsValueOfType(rooted.value(), type);
  }));
  EXPECT_TRUE(std::ranges::all_of(foreign_types, [&rooted](const auto& type) {
    return !type.callable || !runtime::Heap::IsValueOfType(rooted.value(), type);
  }));
}

INSTANTIATE_TEST_SUITE_P(NominalParameters,
                         DynamicNominalIdentityTest,
                         testing::Values(
                             R"(class Token {}
func accept(value: Token): int { return 42; }
func main(): (Token) => int { return accept; }
)",
                             R"(interface Marker { read(): int; }
func accept(value: Marker): int { return 42; }
func main(): (Marker) => int { return accept; }
)"));

void ExpectJsonAnswer(const vm::Program& program, runtime::Isolate& isolate) {
  const auto result = test::ExecuteFutureInteger(program, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST_P(JsonProgramTest, ExecutesWithGcAndReleasesTraversalState) {
  runtime::Isolate isolate(1);
  const auto baseline = isolate.heap().allocated_bytes();
  {
    const auto compiled = CompileSource(GetParam(), isolate);
    for (const auto& diagnostic : compiled.diagnostics) {
      EXPECT_NE(diagnostic.severity, frontend::DiagnosticSeverity::Error)
          << diagnostic.location.line << ':' << diagnostic.message;
    }
    ASSERT_TRUE(compiled.Succeeded());
    ExpectJsonAnswer(compiled.program, isolate);
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().allocated_bytes(), baseline);
}

TEST_P(JsonProgramTest, RestoresImageInFreshIsolate) {
  runtime::Isolate isolate(1);
  const auto loaded = test::ReloadSource(GetParam(), isolate);
  ASSERT_TRUE(loaded) << loaded.error();
  ExpectJsonAnswer(*loaded, isolate);
}

INSTANTIATE_TEST_SUITE_P(ProductionContracts,
                         JsonProgramTest,
                         testing::Values(
                             R"(func main(): int {
  const n: int = Json.parse('42');
  const a: dynamic[] = Json.parse('[1,"Vela",true,null,1.0]');
  const text = Json.stringify(a);
  return n == 42 && text == '[1,"Vela",true,null,1.0]' ? 42 : 0;
})",
                             R"(func main(): int {
  const payload: Map<string, dynamic> = Json.parse('{"b":1,"a":2,"b":3}');
  const b: int = payload.get('b');
  return b == 3 && Json.stringify(payload) == '{"b":3,"a":2}' ? 42 : 0;
})",
                             R"(class User {
  name: string;
  toJson(): Map<string, dynamic> {
    const result = Map<string, dynamic>();
    result.set('name', this.name);
    result.set('items', [1, true, null]);
    return result;
  }
}
func main(): int {
  const users = [User(name: 'Vela'), User(name: 'Language')];
  return Json.stringify(users) ==
    '[{"name":"Vela","items":[1,true,null]},{"name":"Language","items":[1,true,null]}]'
    ? 42 : 0;
})",
                             R"(class Bad {
  toJson(): int { throw Error(message: 'original', code: 'UserError'); }
}
func main(): int {
  try { Json.stringify(Bad()); } catch (error: Error) {
    if (error.code != 'UserError' || error.message != 'original') { return 0; }
    return Json.stringify(Json.parse('42')) == '42' ? 42 : 0;
  }
  return 0;
})",
                             R"(class Cycle {
  toJson(): Cycle { return this; }
}
func main(): int {
  try { Json.stringify(Cycle()); } catch (error: Error) {
    return error.code == 'JsonEncodeError' ? 42 : 0;
  }
  return 0;
})",
                             R"(func main(): int {
  try { Json.parse('9223372036854775808'); } catch (error: Error) {
    return error.code == 'JsonDecodeError' && error.message.includes('byte 0') ? 42 : 0;
  }
  return 0;
})",
                             R"(class Mutator {
  parent: dynamic[];
  toJson(): int {
    this.parent.push(7);
    this.parent.pop();
    return 1;
  }
}
func main(): int {
  const items: dynamic[] = [];
  items.push(Mutator(parent: items));
  try { Json.stringify(items); } catch (error: Error) {
    return error.code == 'ConcurrentModification' ? 42 : 0;
  }
  return 0;
})",
                             R"(func main(): int {
  const branch = [1, 2];
  return Json.stringify([branch, branch]) == '[[1,2],[1,2]]' ? 42 : 0;
})",
                             R"(enum First { value }
enum Second { value }
func main(): int {
  const value: dynamic = First.value;
  const restored: First = value;
  if (!(value is First) || value is int || value is Second || restored != First.value) {
    return 0;
  }
  const keys = Set<dynamic>();
  keys.add(First.value);
  keys.add(First.value);
  keys.add(Second.value);
  keys.add(0);
  if (keys.size != 3 || !keys.has(First.value)) { return 0; }
  try { Json.stringify(value); } catch (error: Error) {
    return error.code == 'JsonEncodeError' ? 42 : 0;
  }
  return 0;
})",
                             R"(enum Colour { red }
func main(): int {
  const values = [Colour.red];
  const erased: dynamic = values;
  if (erased is int[] || !(erased is Colour[])) { return 0; }
  try { Json.stringify(values); } catch (error: Error) {
    return error.code == 'JsonEncodeError' ? 42 : 0;
  }
  return 0;
})",
                             R"(class Mutator {
  parent: Map<string, dynamic>;
  toJson(): int {
    this.parent.set('temp', 7);
    this.parent.delete('temp');
    return 1;
  }
}
func main(): int {
  const items = Map<string, dynamic>();
  items.set('object', Mutator(parent: items));
  try { Json.stringify(items); } catch (error: Error) {
    return error.code == 'ConcurrentModification' ? 42 : 0;
  }
  return 0;
})",
                             R"(func identity(value: int): int { return value; }
func main(): int {
  const erased: dynamic = identity;
  const callable: (int) => int = erased;
  if (erased is (string) => string) { return 0; }
  const callbacks: ((int) => int)[] = [callable];
  const container: dynamic = callbacks;
  return container is ((int) => int)[] && !(container is ((string) => string)[]) &&
    callable(42) == 42 ? 42 : 0;
})",
                             R"(func main(): int {
  const values: dynamic = Json.parse('[40,1.5,"Vela",true]');
  const answer: int = values[0] + 2;
  const number: double = -values[1] * 2.0;
  const text: string = values[2] + 7;
  const flag: bool = values[3] && !false;
  values[0] = answer;
  if (!flag) { return 0; }
  return answer == 42 && number == -3.0 && text == 'Vela7' &&
    Json.stringify(values) == '[42,1.5,"Vela",true]' ? 42 : 0;
})",
                             R"(enum Kind { first, second }
func main(): int {
  const typed = [Kind.first];
  const values: dynamic = typed;
  const item: Kind = values[0];
  values[0] = Kind.second;
  if (item != Kind.first || typed[0] != Kind.second) { return 0; }
  try { values[0] = 0; } catch (error: Error) {
    return error.code == 'TypeError' && typed[0] == Kind.second ? 42 : 0;
  }
  return 0;
})",
                             R"(func main(): int {
  const wrong: dynamic = Json.parse('1.0');
  try { const answer: int = true ? wrong : 0; } catch (error: Error) {
    try { const answer: int = wrong ?? 0; } catch (other: Error) {
      return error.code == 'InvalidCast' && other.code == 'InvalidCast' ? 42 : 0;
    }
  }
  return 0;
})",
                             R"(async func number(): Future<int> { return 42; }
func main(): int {
  const future: dynamic = number();
  const factory: dynamic = Future.value(42);
  const completer: dynamic = Completer<int>();
  return future is Future<int> && !(future is Future<string>) &&
    factory is Future<int> && !(factory is Future<double>) &&
    completer is Completer<int> && !(completer is Completer<string>) ? 42 : 0;
})",
                             R"(enum Flag { yes }
func echo(value: Flag): Flag { return value; }
func main(): int {
  const call: dynamic = echo;
  const actual: Flag = call(Flag.yes);
  if (actual != Flag.yes) { return 0; }
  try { call(0); } catch (error: Error) {
    return error.code == 'InvalidCast' ? 42 : 0;
  }
  return 0;
})",
                             R"(class Adder {
  base: int;
  add(value: int): int { return this.base + value; }
}
func main(): int {
  const method: dynamic = Adder(base: 40).add;
  const captured = 2;
  const lambda: dynamic = (value: int): int => value + captured;
  const first: int = method(2);
  const second: int = lambda(40);
  return first == 42 && second == 42 ? 42 : 0;
})",
                             R"(func first(value: int): int { return 40 + value; }
func second(value: int): int { return value; }
func main(): int {
  let call: dynamic = first;
  const change = (): int => { call = second; return 2; };
  const actual: int = call(change());
  return actual == 42 ? 42 : 0;
})",
                             R"(enum Flag { yes }
class Model { toJson(): Flag { return Flag.yes; } }
func main(): int {
  try { Json.stringify(Model()); } catch (error: Error) {
    return error.code == 'JsonEncodeError' ? 42 : 0;
  }
  return 0;
})",
                             R"(enum Flag { yes }
class Item {
  value: int;
  flag: Flag = Flag.yes;
  private secret: int = 9;
  add(first: int, second: int): int { return this.value + first + second; }
  get twice(): int { return this.value * 2; }
  set twice(value: int): void { this.value = value / 2; }
}
func main(): int {
  const item: dynamic = Item(value: 20);
  if (item.twice != 40 || item.add(second: 1, first: 21) != 42) { return 0; }
  const method = item.add;
  if (method(21, 1) != 42 || !(item.flag is Flag)) { return 0; }
  item.twice = 42;
  if (item.value != 21) { return 0; }
  item.flag = Flag.yes;
  try { item.secret; } catch (error: Error) {
    return error.code == 'TypeError' ? 42 : 0;
  }
  return 0;
})",
                             R"(func first(value: int): int { return value + 40; }
func second(value: int): int { return value; }
class Item {
  callback: (int) => int;
  get action(): (int) => int { return this.callback; }
}
func main(): int {
  const actual = Item(callback: first);
  const item: dynamic = actual;
  const change = (): int => { actual.callback = second; return 2; };
  const result: int = item.action(change());
  const nothing: dynamic = null;
  const fail = (): int => { throw Error(message: 'unexpected'); };
  return result == 42 && nothing?.action(fail()) == null ? 42 : 0;
})",
                             R"(func main(): int {
  const root = Json.parse('{"name":"Vela","values":[40,2]}');
  const name: string = root.get(key: 'name');
  const items = root.get('values');
  items.push(7, 8);
  items.splice(2, 2);
  const copy = items.slice();
  copy.reverse();
  if (items.length != 2 || copy[0] != 2 || name.toLowerCase() != 'vela') { return 0; }
  const has = root.has;
  if (!has('name') || root.size != 2 || root.keys().length != 2) { return 0; }
  root.set('name', 'Vela2');
  return Json.stringify(root) == '{"name":"Vela2","values":[40,2]}' ? 42 : 0;
})",
                             R"(enum Flag { yes }
func main(): int {
  const list: int[] = [1, 2];
  const erased: dynamic = list;
  try { erased.push(3, 'bad'); } catch (error: Error) {
    if (error.code != 'InvalidCast' || list.length != 2) { return 0; }
    const values = Map<string, Flag>();
    values.set('x', Flag.yes);
    const map: dynamic = values;
    const actual: Flag = map.get('x');
    return actual == Flag.yes && erased.slice(1)[0] == 2 ? 42 : 0;
  }
  return 0;
})",
                             R"(class Item {
  stored: int;
  get value(): int { return this.stored; }
  set value(next: int): void { this.stored = next; }
}
func main(): int {
  const original = Item(stored: 20);
  let receiver: dynamic = original;
  const replace = (): int => { receiver = Item(stored: 100); return 20; };
  receiver.value += replace();
  const target: dynamic = original;
  const before: int = target.value++;
  let number: dynamic = 1;
  number += 1;
  ++number;
  const list: dynamic = [1, 2];
  list[0] += 40;
  list[0]++;
  return before == 40 && original.stored == 41 && receiver.stored == 100 &&
    number == 3 && list[0] == 42 ? 42 : 0;
})",
                             R"(func main(): int {
  const bytes: dynamic = ByteArray(length: 8);
  const data: dynamic = ByteData.view(bytes);
  data.setInt32(value: 42, offset: 0, endian: Endian.little);
  const number: int = data.getInt32(0, endian: Endian.little);
  const pattern: dynamic = RegExp('(?<number>[0-9]+)');
  const matches = pattern.allMatches('a42');
  const name: string = matches[0].namedGroup('number');
  return number == 42 && bytes.length == 8 && data.byteLength == 8 &&
    name == '42' && matches[0].start == 1 ? 42 : 0;
})",
                             R"(enum Flag { yes }
async func adopt(value: dynamic): Future<int> { return value; }
async func flag(value: dynamic): Future<Flag> { return value; }
async func main(): Future<int> {
  const scalar = await adopt(40);
  const future = await adopt(Future.value(2));
  const value: Flag = await flag(Flag.yes);
  const passed: Flag = await flag(Future.value(Flag.yes));
  if (scalar + future != 42 || value != Flag.yes || passed != Flag.yes) { return 0; }
  try { await adopt(Future.value('bad')); } catch (error: Error) {
    return error.code == 'InvalidCast' ? 42 : 0;
  }
  return 0;
})",
                             R"(enum Flag { yes }
async func main(): Future<int> {
  const source = Future.value(Flag.yes);
  const wider: Future<dynamic> = source;
  const erased = await wider;
  const through: dynamic = await source.then((value: dynamic): dynamic => value);
  const copied: Flag = await source.then((value: Flag): Flag => value);
  const packed = await Future.wait([source, Future.sync((): Flag => Flag.yes)]);
  const producer = Completer<Flag>();
  const bad: dynamic = 'bad';
  try { producer.complete(bad); } catch (error: Error) {
    if (error.code != 'InvalidCast' || producer.isCompleted) { return 0; }
    producer.complete(Flag.yes);
    const completed: Flag = await producer.future;
    return erased is Flag && through is Flag && copied == Flag.yes &&
      packed[0] == Flag.yes && packed[1] == Flag.yes && completed == Flag.yes ? 42 : 0;
  }
  return 0;
})",
                             R"(class Reader {
  action: () => int;
  get value(): int { return this.action(); }
}
func main(): int {
  let value: dynamic = 42;
  const reader: dynamic = Reader(action: (): int => { value = 'changed'; return 0; });
  if (value is int) {
    reader.value;
    try { const checked: int = value; } catch (error: Error) {
      let number: dynamic = 20;
      const sum: int = number + number++;
      return error.code == 'InvalidCast' && sum == 40 && number == 21 ? 42 : 0;
    }
  }
  return 0;
})",
                             R"(enum Flag { yes }
class Item { value: int = 7; }
func main(): int {
  const values: dynamic[] = [1, 'bad'];
  const item = Item();
  try { values.forEach((value: int): void => { item.value = value; }); }
  catch (error: Error) {
    if (error.code != 'InvalidCast' || item.value != 1) { return 0; }
    const flags: Flag[] = [Flag.yes];
    const identity = flags.map((value: dynamic): dynamic => value);
    const dynamic_values: dynamic[] = [40, 2];
    const integers = dynamic_values.map((value: int): int => value);
    const bad = (acc: int, value: int): dynamic => 'wrong';
    try { integers.reduce(bad, 0); } catch (failure: Error) {
      return failure.code == 'InvalidCast' && identity[0] is Flag &&
        integers[0] + integers[1] == 42 ? 42 : 0;
    }
  }
  return 0;
})",
                             R"(func main(): int {
  const values: dynamic = Json.parse('[40,2]');
  const mapped = values.map((value: int): string => value.toString());
  const total: int = values.reduce((acc: int, value: int): int => acc + value, 0);
  let sum = 0;
  values.forEach((value: int): void => { sum += value; });
  const found: int = values.findIndex((value: int): bool => value == 2);
  return mapped is string[] && !(mapped is dynamic[]) && mapped[0] == '40' && total == 42 && sum == 42 &&
    values.some((value: int): bool => value == 40) &&
    values.every((value: int): bool => value > 0) && found == 1 ? 42 : 0;
})",
                             R"(func main(): int {
  const typed = [2, 10];
  const erased: dynamic = typed;
  erased.sort();
  const mixed: dynamic = [30, 2, 20];
  mixed.sort((left: int, right: int): double => (left - right).toDouble());
  const empty: dynamic = [];
  const result = empty.map((value: int): int => value);
  return typed[0] == 10 && mixed[0] == 2 && mixed[2] == 30 && result is int[] && result.length == 0 ? 42 : 0;
})",
                             R"(enum Flag { yes }
class Adder {
  base: int;
  add(value: int): int { return this.base + value; }
}
func main(): int {
  const flags = [Flag.yes];
  const erased: dynamic = flags;
  const copy = erased.map((value: Flag): Flag => value);
  const values: dynamic = [40];
  const result = values.map(Adder(base: 2).add);
  const flag: Flag = copy[0];
  return copy is Flag[] && !(copy is int[]) && flag == Flag.yes && result[0] == 42 ? 42 : 0;
})",
                             R"(func main(): int {
  const source = [1, 2];
  const values: dynamic = source;
  const mutate = (value: int): int => { source.push(7); source.pop(); return value; };
  try { values.map(mutate); } catch (error: Error) {
    if (error.code != 'ConcurrentModification') { return 0; }
    const empty: dynamic = source.slice(0, 0);
    const invalid = (left: int, right: int): dynamic => 0;
    try { empty.sort(invalid); } catch (failure: Error) {
      return failure.code == 'TypeError' ? 42 : 0;
    }
  }
  return 0;
})",
                             R"(enum Flag { yes }
async func main(): Future<int> {
  const owner = Completer<Flag>();
  const erased: dynamic = owner;
  const future: dynamic = erased.future;
  const chained = future.then((value: Flag): int => value == Flag.yes ? 42 : 0);
  let rejected = false;
  try { erased.complete(1); } catch (error: Error) { rejected = true; }
  if (!rejected || erased.isCompleted) { return 0; }
  const complete = erased.complete;
  const flag: dynamic = Flag.yes;
  complete(flag);
  const result: int = await chained;
  return erased.isCompleted && (future is Future<Flag>) && (chained is Future<int>) ? result : 0;
})",
                             R"(async func main(): Future<int> {
  const source: dynamic = Future.value(42);
  const preserved = source.whenComplete((): void => {});
  const ready = preserved.timeout(milliseconds: 1000);
  const owner = Completer<int>();
  const pending: dynamic = owner.future;
  const timed = pending.timeout(0, onTimeout: (): int => 42);
  const first: int = await ready;
  const second: int = await timed;
  const token: dynamic = CancellationToken();
  const cancel = token.cancel;
  cancel();
  const timer: dynamic = Timer.after(1000, (): void => {});
  timer.cancel();
  return first == second && token.isCancelled && (timed is Future<int>) ? 42 : 0;
})",
                             R"(async func main(): Future<int> {
  const problem: dynamic = Error(message: 'bad');
  let caught = 0;
  try { throw problem; } catch (error: Error) { caught += 1; }
  const failed: dynamic = Future.error<int>(problem);
  const recovered = failed.catchError((error: Error): int => 42);
  const value: int = await recovered;
  const owner: dynamic = Completer<int>();
  const next: dynamic = owner.future;
  owner.completeError(problem);
  const repaired = next.catchError((error: Error): int => 42);
  const other: int = await repaired;
  try { throw 1 as dynamic; } catch (error: Error) { caught += 1; }
  try { Future.error<int>(1 as dynamic); } catch (error: Error) { caught += 1; }
  return value == other && caught == 3 ? 42 : 0;
})",
                             R"(enum Flag { yes }
async func main(): Future<int> {
  const flag: dynamic = Flag.yes;
  const value: Future<Flag> = Future.value(flag);
  const result = await value;
  let caught = false;
  try { const bad: Future<int> = Future.value(flag); } catch (error: Error) { caught = true; }
  const owner = Completer<void>();
  const nothing: dynamic = null;
  owner.complete(nothing);
  await owner.future;
  return result == Flag.yes && caught ? 42 : 0;
})",
                             R"(async func main(): Future<int> {
  const owner = Completer<int>();
  const source: dynamic = owner.future;
  let caught = 0;
  try { source.then((value: string): int => 0); } catch (error: Error) { caught += 1; }
  try { source.catchError((error: int): int => 0); } catch (error: Error) { caught += 1; }
  const failed: dynamic = Future.error<int>(Error(message: 'bad'));
  const result = failed.catchError((error: Error): dynamic => 'wrong');
  try { await result; } catch (error: Error) { caught += 1; }
  owner.complete(42);
  return caught == 3 ? 42 : 0;
})",
                             R"(func main(): int {
  const value: dynamic = 42;
  const values: dynamic[] = [3, 1, 2];
  values.sort();
  const erased: dynamic = values;
  erased.sort();
  let caught = false;
  try { const text = '${[1] as dynamic}'; } catch (error: Error) { caught = true; }
  return '${value}' == '42' && values.join(',') == '1,2,3' && caught ? 42 : 0;
})",
                             R"(async func main(): Future<int> {
  const source: dynamic = Future.value(1);
  const mapped = source.then((value: int): Future<int>? => null);
  const retained: Future<int?> = mapped;
  const result = await retained;
  const direct: Future<int?> = Future.value(1).then((value: int): Future<int>? => null);
  const other = await direct;
  return result == null && other == null && (mapped is Future<int?>) ? 42 : 0;
})",
                             R"(async func done(value: dynamic): Future<void> { return value; }
async func main(): Future<int> {
  const owner: dynamic = Completer<void>();
  owner.complete();
  await owner.future;
  await done(null);
  await done(Future.value<void>(null as dynamic));
  try { await done(42); } catch (error: Error) { return 42; }
  return 0;
})",
                             R"(class Box {
  stored: dynamic = 20;
  reads: int = 0;
  writes: int = 0;
  get value(): dynamic { this.reads += 1; return this.stored; }
  set value(input: dynamic): void { this.writes += 1; this.stored = input; }
  static total: dynamic = 40;
}
func main(): int {
  const box = Box();
  box.value += 2;
  const erased: dynamic = box;
  erased.value += 20;
  Box.total += 2;
  let count: dynamic = 1;
  const result = count += count++;
  return box.stored == 42 && box.reads == 2 && box.writes == 2 && Box.total == 42 &&
      result == 2 && count == 2 ? 42 : 0;
})",
                             R"(func main(): int {
  const source: dynamic = [20, 22];
  const result: int? = source.reduce((total: int?, value: int): int? => (total ?? 0) + value, null);
  const empty: dynamic = [];
  const retained: int? = empty.reduce((total: int?, value: dynamic): int? => total, null);
  return result == 42 && retained == null ? 42 : 0;
})"));

}  // namespace
}  // namespace vela::compiler
