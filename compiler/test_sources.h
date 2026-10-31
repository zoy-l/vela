#pragma once

#include <string_view>

namespace vela::compiler::test {

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
