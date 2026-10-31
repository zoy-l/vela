#pragma once

#include <string_view>

namespace vela::compiler::test {

inline constexpr std::string_view kMapLiteralSource = R"(
let trace = '';
func key(value: string): string { trace += 'k' + value; return value; }
func item(value: int): int { trace += 'v' + value; return value; }
func pair<K, V>(key: K, value: V): Map<K, V> { return {key: value}; }
class Box { value: int; values: Map<string, int> = {'answer': 42}; }
enum First { key }
enum Second { key }
export func main(): int {
  trace = '';
  const ordered = {key('a'): item(1), key('b'): item(2), key('a'): item(3),};
  if (trace != 'kav1kbv2kav3' || ordered.size != 2 || ordered.get('a') != 3 ||
      ordered.keys().join() != 'a,b') { return 0; }
  let changing = 'before';
  const saved = {changing: (changing = 'after').length};
  if (!saved.has('before') || saved.has('after')) { return 0; }
  const empty: Map<string, int> = {};
  const another: Map<string, int> = {};
  const identities = {empty: 1, another: 2};
  empty.set('changed', 1);
  if (identities.size != 2 || another.size != 0) { return 0; }
  const nested: Map<string, int[][]> = {'items': [[], [40, 2]]};
  const arrays = nested.get('items');
  if (arrays == null || arrays[0].length != 0) { return 0; }
  const object = Box(value: 42);
  const references = {object: 'a' + 'b'};
  if (references.has(Box(value: 42)) || references.get(object) != 'ab') { return 0; }
  const shared = pair('object', object);
  object.value = 41;
  const found = shared.get('object');
  if (found == null || found.value != 41 || found.values.get('answer') != 42) { return 0; }
  const erased: Map<dynamic, dynamic> = {1: 'int', 1.0: 'double', null: true,
                                       First.key: 1, Second.key: 2};
  if (erased.size != 5 || erased.get(1) != 'int' || erased.get(1.0) != 'double') { return 0; }
  const infinity = 1e308 * 1e308;
  const nan = infinity - infinity;
  const numbers = {0.0: 1, -0.0: 2, nan: 3, -nan: 4};
  if (numbers.size != 2 || numbers.get(0.0) != 2 || numbers.get(nan) != 4) { return 0; }
  const nullable: Map<string?, int?> = {null: null, 'answer': 42};
  if (!nullable.has(null)) { return 0; }
  let captured = 0;
  const callbacks: Map<string, () => int> = {'next': () => { captured++; return captured; }};
  const next = callbacks.get('next');
  if (next == null || next() != 1 || next() != 2 || captured != 2) { return 0; }
  const growing: Map<int, string> = {};
  for (let index = 0; index < 256; index++) { growing.set(index, '${index}'); }
  if (growing.size != 256 || growing.get(255) != '255') { return 0; }
  trace = '';
  return nullable.get('answer') ?? 0;
}
)";

inline constexpr std::string_view kMapLiteralInitializationSource = R"(
let attempts = 0;
class Failure {}
func next(): int { attempts++; if (attempts == 1) { throw Failure(); } return 40; }
const values: Map<string, int> = {'answer': next(), 'extra': 2};
const left: Map<string, int> = {'cycle': right.get('cycle') ?? 0};
const right: Map<string, int> = {'cycle': left.get('cycle') ?? 0};
export func main(): int {
  try { values.get('answer'); } catch (failure: Failure) { }
  if (attempts != 1) { return 0; }
  const answer = values.get('answer');
  if (attempts != 2 || answer != 40) { return 0; }
  try { left.get('cycle'); }
  catch (error: Error) {
    if (error.code == 'CyclicInitialization') { return (answer ?? 0) + (values.get('extra') ?? 0); }
  }
  return 0;
}
)";

inline constexpr std::string_view kMapLiteralAsyncSource = R"(
let calls = 0;
async func key(value: string): Future<string> { calls++; await sleep(1); return value; }
async func item(value: int): Future<int> { calls++; await sleep(1); return value; }
export async func main(): Future<int> {
  const values = {await key('answer'): await item(40), await key('answer'): await item(42)};
  if (calls != 4 || values.size != 1) { return 0; }
  try {
    const bad: dynamic = 'wrong';
    const failed = {bad as int: await item(0), 2: await item(0)};
  } catch (error: Error) {
    if (calls == 4) { return values.get('answer') ?? 0; }
  }
  return 0;
}
)";

inline constexpr std::string_view kGenericMethodsSource = R"(
interface Mapper<T> { map<U>(value: U, operation: (U) => T): T; }
class MapperImpl<T> implements Mapper<T> {
  map<V>(value: V, operation: (V) => T): T { return operation(value); }
}
class Box<T> {
  value: T;
  transform<U>(operation: (T) => U): U { return operation(this.value); }
  static identity<U>(value: U): U { return value; }
}
export func main(): int {
  const box = Box<int>(value: 20);
  const operation: ((int) => int) => int = box.transform;
  const mapper: Mapper<int> = MapperImpl<int>();
  const mapped: (string, (string) => int) => int = mapper.map;
  const text: Mapper<string> = MapperImpl<string>();
  const copied = text.map(2, value => '${value}');
  return operation(value => value) + mapped(copied, value => value.length) +
         Box<int>.identity(21);
}
)";

inline constexpr std::string_view kWrappedGenericMethodValuesSource = R"(
class Operations { identity<T>(value: T): T { return value; } }
let calls: int = 0;
func make(): Operations { calls += 1; return Operations(); }
func apply<T>(operation: (T) => T, value: T): T { return operation(value); }
func applyAll<T>(operations: ((T) => T)[], value: T): T { return operations[0](value); }
export func main(): int {
  const absent: ((int) => int)? = null;
  const first = apply(match(make()) { let owner => owner.identity }, 10);
  const second = apply(absent ?? make().identity, 10);
  const third = applyAll([make().identity], 10);
  const fourth = [8].map(match(make()) { let owner => owner.identity })[0];
  return first + second + third + fourth + calls;
}
)";

inline constexpr std::string_view kGenericAsyncMethodsSource = R"(
class Failure<T> { value: T; }
interface Delayed {
  later<T>(value: T): Future<T>;
  fail<T>(value: T): Future<T>;
}
class Worker implements Delayed {
  async later<T>(value: T): Future<T> { await sleep(1); return value; }
  async fail<T>(value: T): Future<T> {
    await sleep(1);
    throw Failure<T>(value: value);
  }
}
export async func main(): Future<int> {
  const worker: Delayed = Worker();
  const operation: (string) => Future<string> = worker.later;
  const text = await operation('ab');
  try { await worker.fail(40); }
  catch (failure: Failure<int>) { return failure.value + text.length; }
  return 0;
}
)";

inline constexpr std::string_view kRecursiveStringGcSource = R"(
func descend(depth: int): int {
  const text = '${depth}';
  if (depth == 0) { return text.length + 41; }
  const answer = descend(depth - 1);
  return text.length > 0 ? answer : 0;
}
export func main(): int { return descend(512); }
)";

inline constexpr std::string_view kUnicodeConstructionSource = R"(
export func main(): int {
  const text = 'A中😀e\u0301\0';
  const joined = [text, text].join('界');
  const absent: string? = null;
  const formatted = '${text}/${42}/${true}/${absent}';
  const numbers = [12, 34].join('中');
  const builder = String(); builder.append(text); builder.append('ß');
  const snapshot = builder.toString();
  const forward = String(); const backward = String();
  const dynamicText: dynamic = text;
  for (let i = 0; i < dynamicText.length; i++) { forward.append(dynamicText[i]); }
  for (let i = text.length - 1; i >= 0; i--) { backward.append(text[i]); }
  return text.length == 6 && joined.length == 13 && formatted.length == 19 &&
         numbers.length == 5 && snapshot.length == 7 && snapshot.toUpperCase().length == 8 &&
         'İßﬃ'.toLowerCase().length == 4 && 'ßﬃ'.toUpperCase().length == 5 &&
         forward.toString() == text && backward.toString() == '\0\u0301e😀中A' &&
         joined.slice(6, 9) == '界A中' ? 42 : 0;
}
)";

}  // namespace vela::compiler::test
