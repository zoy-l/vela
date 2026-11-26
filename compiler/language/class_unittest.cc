#include <algorithm>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "compiler/test_support/execution_expectations.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

class ClassDispatchTest : public testing::TestWithParam<std::string_view> {};

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
        ClassError{"class A { text: string?; } func main(): A { return A(); }",
                   "missing argument 'text'"},
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
        ClassError{
            "class A {  #get(): int { return 1; } } func main(): int { return A().#get(); }",
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
