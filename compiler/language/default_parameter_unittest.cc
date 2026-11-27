#include <string_view>

#include <gtest/gtest.h>

#include "compiler/test_support/execution_expectations.h"

namespace vela::compiler {
namespace {

class DefaultParameterTest : public testing::TestWithParam<std::string_view> {};

TEST_P(DefaultParameterTest, UsesDefaultsOnlyForOmittedArgumentsUnderGcPressure) {
  test::ExpectRunAnswer(GetParam());
}

TEST_P(DefaultParameterTest, RestoresDefaultContractsWithoutCompilerMetadata) {
  test::ExpectRestoredAnswer(GetParam());
}

INSTANTIATE_TEST_SUITE_P(CallsAndValues,
                         DefaultParameterTest,
                         testing::Values(
                             R"(
func read(value: int?): int { return value ?? 21; }
func main(): int {
  const short: () => int = read;
  const closure: () => int = (value: int?): int => value ?? 21;
  return read() == 21 && read(null) == 21 && read(7) == 7 ? short() + closure() : 0;
}
)",
                             R"(
class Drawing { value: int = 42; }
class View {
  overlayKey: string?;
  surface: Drawing?;
  #hidden: int?;
  static shared: string?;
  read(): int { return this.overlayKey == null && this.surface == null &&
      this.#hidden == null && View.shared == null ? 42 : 0; }
}
func main(): int { return View().read(); }
)",
                             R"(
type Maybe<T> = T?;
class Box<T> { value: Maybe<T>; }
func read<T>(value: T?): int { return value == null ? 21 : 0; }
func main(): int { return Box<int>().value == null ? read<int>() + read<string>() : 0; }
)",
                             R"(
type Maybe<T> = T?;
func read<T>(value: Maybe<T>): int { return value == null ? 21 : 0; }
func main(): int { return read<int>() + read<string>(); }
)",
                             R"(
class Box<T> { value: T; }
func read<T>(value: T): int { return value == null ? 21 : 0; }
func main(): int { return Box<int?>().value == null ? read<int?>() + read<string?>() : 0; }
)",
                             R"(
interface Reader { read(value: int?): int; }
class Base implements Reader { read(value: int?): int { return value ?? 20; } }
class Child extends Base { override read(value: int? = 21): int { return value ?? 1; } }
func use(reader: Reader): int { return reader.read(); }
func main(): int { const value = Child(); return use(value) + value.read(); }
)",
                             R"(
class Base { value: int?; }
class Child extends Base { text: string?; }
class Bound { value: int?; constructor(this.value) {} }
class Forward extends Base { constructor(super.value) {} }
func main(): int {
  return Child().value == null && Child().text == null && Bound().value == null &&
      Forward().value == null && Bound(42).value == 42 ? 42 : 0;
}
)",
                             R"(
class Readonly {
  const #value: int?;
  constructor(this.#value) {}
  read(): int? { return this.#value; }
}
func main(): int {
  return Readonly().read() == null ? (Readonly(42).read() ?? 0) : 0;
}
)",
                             R"(
class Box { value: int?; constructor() {} }
class Local { value: int?; constructor(value: int?) { this.value = value; } }
func main(): int { return Box().value == null && Local().value == null ? 42 : 0; }
)",
                             R"(
func read(value: int = 21): int { return value; }
func main(): int {
  const named: () => int = read;
  const closure: () => int = (value: int = 21): int => value;
  const first: dynamic = named;
  const second: dynamic = closure;
  if (first is () => int && second is () => int) { return named() + closure(); }
  return 0;
}
)",
                             R"(
class Counter { toJson(value: int = 42): int { return value; } }
func main(): int { return JSON.stringify(Counter()) == '42' ? 42 : 0; }
)",
                             R"(
type UpdateAction = () => void;
let actions = 0;
class Component {
  value: int = 0;
  update(action: UpdateAction? = null): void {
    if (action != null) { action(); }
    this.value += 21;
  }
}
func main(): int {
  const component = Component();
  component.update();
  component.update((): void => { actions++; });
  return actions == 1 ? component.value : 0;
}
)",
                             R"(
enum Mode { first, second }
func choose(mode: Mode = Mode.second): int { return mode == Mode.second ? 21 : 0; }
func main(): int {
  const bound: () => int = choose;
  return bound() + choose(Mode.second);
}
)",
                             R"(
func main(): int {
  const short: () => int = (value: int = 21): int => value;
  const full: (int) => int = (value = 0): int => value;
  let value = 0;
  return short() + full((value = 21));
}
)",
                             R"(
func update(action: int? = null): int { return action ?? 21; }
func main(): int { return update() + update(21); }
)",
                             R"(
let calls = 0;
func initial(): int { calls++; return 20; }
func read(value: int? = initial()): int { return value ?? 2; }
func main(): int {
  const first = read();
  const second = read(null);
  const third = read(20);
  return calls == 1 ? first + second + third : 0;
}
)",
                             R"(
let order = 0;
func mark(value: int): int { order = order * 10 + value; return value; }
func read(first: int = mark(1), second: int = first + mark(2), third: int = mark(3)): int {
  return first + second + third;
}
func main(): int { return read(third: mark(3)) == 7 && order == 312 ? 42 : 0; }
)",
                             R"(
interface Reader { read(value: int = 1): int; }
class Base implements Reader { read(value: int = 2): int { return value; } }
class Child extends Base { override read(value: int = 21): int { return value; } }
func use(base: Base, reader: Reader): int { return base.read() + reader.read(); }
func main(): int { const value = Child(); return use(value, value); }
)",
                             R"(
class Counter {
  value: int = 20;
  read(amount: int = this.value): int { return amount; }
  static extra(value: int = 2): int { return value; }
}
func main(): int {
  const value = Counter();
  const empty: () => int = value.read;
  const full: (int) => int = value.read;
  return empty() + full(20) + Counter.extra();
}
)",
                             R"(
func read(value: int = 21): int { return value; }
func main(): int {
  const empty: () => int = read;
  const full: (int) => int = read;
  return empty() + full(21);
}
)",
                             R"(
class Counter { read(value: int = 21): int { return value; } }
func main(): int {
  const object: dynamic = Counter();
  const bound = object.read;
  return (object.read() + bound()) as int;
}
)",
                             R"(
func read<T>(value: T? = null): T? { return value; }
class Reader { read<T>(value: T? = null): T? { return value; } }
func main(): int {
  if (read<int>() != null || Reader().read<string>() != null) { return 0; }
  return (read<int>(20) ?? 0) + (Reader().read<int>(22) ?? 0);
}
)",
                             R"(
func read(value: int = 20, callback: () => int = (): int => value): int {
  value = 21;
  return callback();
}
func main(): int { return read() + read(22); }
)",
                             R"(
class Counter {
  value: int;
  constructor(value: int = 42) { this.value = value; }
}
func main(): int { return Counter().value; }
)",
                             R"(
[Reflect]
class Counter { read(value: int = 21): int { return value; } }
func main(): int {
  const info = Reflect.typeInfo<Counter>();
  const method = info.findMethod('read');
  if (method == null) { return 0; }
  const instance = Counter();
  const supplied = CallArguments(items: [CallArgument(value: 21)]);
  for (let index = 0; index < 3; index++) {
    if ((method.invoke(instance, supplied) as int) != 21) { return 0; }
  }
  return (method.invoke(instance, CallArguments()) as int) + Counter().read();
}
)"));

TEST(DefaultParameterAsyncTest, OptionalCallbacksAndAsyncDefaultsKeepTheirArguments) {
  test::ExpectFutureAnswer(R"(
func read(value: int = 21): int { return value; }
async func main(): Future<int> {
  return (await Future.sync(read)) + (await Future.value(21).then(read));
}
)",
                           test::kExpectedAnswer);
}

class InvalidDefaultParameterTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InvalidDefaultParameterTest, RejectsInvalidDefaultContracts) {
  test::ExpectRejectedCheck(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    TypesAndVisibility,
    InvalidDefaultParameterTest,
    testing::Values("func read(value: int = null): int { return value; }",
                    "class Box { const value: int?; constructor() { this.value = 1; } }",
                    "func read(first: int = second, second: int = 1): int { return first; }",
                    "external func read(value: int = 1): int;",
                    "class Base { read(value: int = 1): int { return value; } } "
                    "class Child extends Base { override read(value: int): int { return value; } }",
                    "class Counter { value: int; constructor(value: int = this.value) { this.value "
                    "= value; } }"));

}  // namespace
}  // namespace vela::compiler
