#include <algorithm>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/test_support/checker_test_support.h"
#include "compiler/test_support/execution_expectations.h"

namespace vela::compiler {
namespace {

class SafeConstructionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(SafeConstructionTest, InitializesReadonlyAndDeferredFieldsUnderGc) {
  test::ExpectRunAnswer(GetParam());
}

TEST_P(SafeConstructionTest, PreservesTheContractWithoutCompilerMetadata) {
  test::ExpectRestoredAnswer(GetParam());
}

INSTANTIATE_TEST_SUITE_P(ConstructionRules,
                         SafeConstructionTest,
                         testing::Values(
                             R"(
class Root { get value(): int { return 42; } }
class Base extends Root {
  constructor { this.observe(); }
  observe(): void { const value = super.value; }
}
class Child extends Base {
  #ready: int;
  constructor() { super(); this.#ready = 42; }
  override get value(): int { return this.#ready; }
  read(): int { return this.#ready; }
}
func main(): int { return Child().read(); }
)",
                             R"(
class Base {
  const #value: int = 10;
  #read(): int { return this.#value; }
  readBase(): int { const bound = this.#read; return bound(); }
  readOther(other: Base): int { return other.#value; }
}
class Child extends Base {
  const #value: int = 20;
  value: int = 2;
  #read(): int { return this.#value; }
  readChild(): int { return this.#read() + this.value; }
}
func main(): int {
  const child = Child();
  return child.readBase() + child.readOther(child) + child.readChild();
}
)",
                             R"(
class Box {
  const #value: int;
  #constructor(this.#value);
  static create(value: int): Box { return Box(value: value); }
  read(other: dynamic): int { return other.#value; }
  add(other: dynamic): void { other.#count += 2; }
  #count: int = 0;
  count(): int { return this.#count; }
}
func main(): int {
  const box = Box.create(40);
  box.add(box);
  try { box.read(1); return 0; } catch (error: Error) {}
  return box.read(box) + box.count();
}
)",
                             R"(
class Box<T> {
  const #value: T;
  constructor(this.#value);
  #identity<U>(value: U): U { return value; }
  read(): T { return this.#identity(this.#value); }
}
class Child<U> extends Box<U> {
  #identity<T>(value: T): T { return value; }
  pass(value: U): U { const identity: (U) => U = this.#identity; return identity(value); }
}
func main(): int { const box = Child<int>(value: 40); return box.read() + box.pass(2); }
)",
                             R"(
class Box {
  #value: int = 0;
  get #amount(): int { return this.#value; }
  set #amount(value: int): void { this.#value = value; }
  static #offset: int = 2;
  static #adjust(value: int): int { return value + Box.#offset; }
  run(other: dynamic): int { other.#amount = 39; other.#amount++; return Box.#adjust(other.#amount); }
}
func main(): int { const box = Box(); return box.run(box); }
)",
                             R"(
class Box {
  #value: int = 42;
  read(other: Box?): int { return other?.#value ?? 0; }
  dynamicRead(other: dynamic): int { return other?.#value ?? 0; }
}
func main(): int {
  const box = Box();
  return box.read(null) + box.dynamicRead(null) + box.dynamicRead(box);
}
)",
                             R"(
[Reflect]
class Base {
  const #value: int = 20;
  value: int = 1;
  base(): int { return this.#value; }
}
[Reflect]
class Child extends Base {
  const #value: int = 21;
  child(): int { return this.#value; }
}
func main(): int {
  const child = Child();
  const publicField = Reflect.typeInfo<Child>().findField('value');
  if (publicField == null || publicField.getValue(child) != 1) { return 0; }
  return child.base() + child.child() + child.value;
}
)",
                             R"(
class Panel {
  const title: string;
  const closable: bool;
  constructor(this.title, this.closable);
}
class TitledPanel extends Panel {
  const prefix: string;

  #fullHeader: string;

  constructor(this.prefix, rawTitle: string, closable: bool = true) {
    super(title: '${this.prefix}: ${rawTitle}', closable: closable);
    this.#fullHeader = '[${this.title}]';
  }
  get header(): string { return this.#fullHeader; }
}
func main(): int {
  const panel = TitledPanel('Vela', 'panel');
  return panel.header == '[Vela: panel]' && panel.closable ? 42 : 0;
}
)",
                             R"(
class Base { value: int; }
class Child extends Base {

  const #header: string;

  constructor { this.#header = '${this.value}'; }
  read(): string { return this.#header; }
}
func main(): int { return Child(value: 42).read() == '42' ? 42 : 0; }
)",
                             R"(
class Base {
  title: string = 'panel';
  constructor { if (this.ready() != 2) { throw Error(message: 'not ready'); } }
  ready(): int { return 0; }
}
class Child extends Base {
  const prefix: int;

  const #header: string;

  constructor(this.prefix) { super(); this.#header = this.title; }
  override ready(): int { return this.prefix; }
  read(): string { return this.#header; }
}
func main(): int { return Child(2).read() == 'panel' ? 42 : 0; }
)",
                             R"(
class Counter {
  const value: int;
  constructor(flag: bool) { if (flag) { this.value = 40; } else { this.value = 42; } }
}
func main(): int { return Counter(false).value; }
)",
                             R"(
class Counter { const values: int[]; constructor(this.values); }
func main(): int { const counter = Counter([40]); counter.values.push(2); return counter.values[0] + counter.values[1]; }
)",
                             R"(
let calls = 0;
func initial(): int { calls++; return 40; }
class Counter { const value: int = initial(); constructor(this.value); }
func main(): int { const a = Counter(); const b = Counter(42); return calls == 1 && a.value == 40 ? b.value : 0; }
)",
                             R"(
class Base {}
class Counter extends Base {

  const #value: int;

  constructor(flag: bool) { super(); if (flag) { this.#value = 42; return; } this.#value = 42; }
  read(): int { return this.#value; }
}
func main(): int { return Counter(false).read(); }
)",
                             R"(
class Counter {
  const value: int;
  constructor() { while (true) { this.value = 42; break; } }
}
func main(): int { return Counter().value; }
)",
                             R"(
class Counter {
  const value: int;
  constructor(flag: bool) {
    try { if (flag) { throw Error(message: 'failed'); } this.value = 42; }
    catch (error: Error) { throw error; }
  }
}
func main(): int { return Counter(false).value; }
)",
                             R"(
class Base<T> { value: T; }
class Counter<U> extends Base<U> {

  const #stored: U;

  constructor(super.value) { this.#stored = this.value; }
  read(): U { return this.#stored; }
}
func main(): int { return Counter(42).read(); }
)",
                             R"(
class Counter { static const value: int = 42; }
func main(): int { return Counter.value; }
)",
                             R"(
[Reflect]
class Counter { const value: int = 40; const values: int[] = [1, 2]; }
func main(): int {
  const counter = Counter();
  const dynamicCounter: dynamic = counter;
  try { dynamicCounter.value = 1; return 0; }
  catch (error: Error) { if (error.code != 'TypeError') { return 0; } }
  try { dynamicCounter.value = dynamicCounter.value + 1; return 0; }
  catch (error: Error) { if (error.code != 'TypeError') { return 0; } }
  const field = Reflect.typeInfo<Counter>().findField('value');
  if (field == null) { return 0; }
  if (field.getValue(counter) != 40) { return 0; }
  try { field.setValue(counter, 1); return 0; }
  catch (error: Error) { if (error.code != 'ReflectionAccessDenied') { return 0; } }
  dynamicCounter.values[0] = 41;
  return counter.values[0] == 41 ? counter.value + counter.values.length : 0;
}
)"));

struct RejectedConstruction final {
  std::string_view source;
  std::string_view message;
};

class UnsafeConstructionTest : public testing::TestWithParam<RejectedConstruction> {};

TEST_P(UnsafeConstructionTest, RejectsTheUnsafePath) {
  const auto& parameter = GetParam();
  const auto checked = frontend::CheckTestSource(parameter.source);
  EXPECT_FALSE(checked.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [&](const auto& diagnostic) {
    return diagnostic.message.contains(parameter.message);
  })) << frontend::DiagnosticsText(checked.diagnostics);
}

INSTANTIATE_TEST_SUITE_P(
    InitializationProofs,
    UnsafeConstructionTest,
    testing::Values(
        RejectedConstruction{R"(
class Base {
  constructor { const bound: (int) => int = this.identity; }
  identity<T>(value: T): T { return value; }
}
class Child extends Base {
  #ready: int;
  constructor() { super(); this.#ready = 42; }
}
)",
                             "initialized before super"},
        RejectedConstruction{"class Counter { private value: int = 1; }",
                             "expected ':' after field name"},
        RejectedConstruction{"class A { private: value: int; }",
                             "expected ';' after field declaration"},
        RejectedConstruction{"class A { public: value: int; }",
                             "expected ';' after field declaration"},
        RejectedConstruction{
            "class A { #x: int = 1; } class B extends A { read(): int { return this.#x; } }",
            "unknown field"},
        RejectedConstruction{"class A { #f(): int { return 1; } } class B extends A { override "
                             "#f(): int { return 2; } }",
                             "cannot override"},
        RejectedConstruction{"class A { #f(): int { return 1; } } class B extends A { f(): int { "
                             "return super.#f(); } }",
                             "through super"},
        RejectedConstruction{"class A { static #x: int = 1; } class B extends A { static f(): int "
                             "{ return B.#x; } }",
                             "declared static member"},
        RejectedConstruction{"class A { #x: int = 1; read(a: dynamic): int { return a.#x; } } func "
                             "f(a: dynamic): int { return a.#x; }",
                             "declaring class scope"},
        RejectedConstruction{
            "class Counter { const value: int = 1; reset(): void { this.value = 2; } }",
            "const field"},
        RejectedConstruction{
            "class Counter { const value: int = 1; reset(): void { this.value++; } }",
            "const field"},
        RejectedConstruction{
            "class Counter { const value: int = 1; constructor { this.value += 2; } }",
            "const field"},
        RejectedConstruction{
            "class Counter { const value: int; constructor(this.value) { this.value = 2; } }",
            "only be initialized once"},
        RejectedConstruction{"class Counter { const value: int; constructor(flag: bool) { if "
                             "(flag) { this.value = 1; } this.value = 2; } }",
                             "only be initialized once"},
        RejectedConstruction{"class Counter { const value: int; constructor(flag: bool) { while "
                             "(flag) { this.value = 1; } } }",
                             "initialized repeatedly"},
        RejectedConstruction{
            "class Counter { const value: int; constructor() { try { this.value = 1; throw "
            "Error(message: 'failed'); } catch (error: Error) { this.value = 2; } } }",
            "only be initialized once"},
        RejectedConstruction{"class Counter { const value: int; constructor() { try { this.value = "
                             "1; } finally { this.value = 2; } } }",
                             "only be initialized once"},
        RejectedConstruction{R"(
class Base { constructor { this.ready(); } ready(): void {} }
class Child extends Base {
 #header: string;

  constructor() { super(); this.#header = 'ready'; }
  override ready(): void { print(this.#header); }
}
)",
                             "initialized before super"},
        RejectedConstruction{R"(
class Base { constructor { this.ready(); } ready(): void {} }
class Child extends Base {

  #header: string;
  #read(): string { return this.#header; }

  constructor() { super(); this.#header = 'ready'; }
  override ready(): void { print(this.#read()); }
}
)",
                             "initialized before super"},
        RejectedConstruction{R"(
let escaped: dynamic = null;
class Base { constructor { escaped = this; } }
class Child extends Base {  #header: string;  constructor() { super(); this.#header = 'ready'; } }
)",
                             "initialized before super"},
        RejectedConstruction{R"(
class Base { constructor { const f = () => this.ready(); } ready(): void {} }
class Child extends Base {  #header: string;  constructor() { super(); this.#header = 'ready'; } }
)",
                             "initialized before super"},
        RejectedConstruction{R"(
class Base { constructor { this.ready(); } ready(): void {} }
class Parent extends Base {
 #header: string;

  constructor() { super(); this.#header = 'ready'; }
  read(): string { return this.#header; }
  override ready(): void {}
}
class Child extends Parent { override ready(): void { print(this.read()); } }
)",
                             "construction of 'Child'"},
        RejectedConstruction{R"(
class Base<T> { constructor { this.ready(); } ready(): void {} }
class Parent<T> extends Base<T> {
 #header: string;

  constructor() { super(); this.#header = 'ready'; }
  read(): string { return this.#header; }
  override ready(): void {}
}
class Child<U> extends Parent<U> { override ready(): void { print(this.read()); } }
)",
                             "initialized before super"},
        RejectedConstruction{R"(
class Base {}
class Child extends Base {  #header: string;  constructor(flag: bool) { super(); if (flag) { this.#header = 'ready'; } } }
)",
                             "must be initialized"},
        RejectedConstruction{R"(
class Base { const value: int = 1; }
class Child extends Base { constructor() { try { super(); } catch (error: Error) { super(); } } }
)",
                             "exactly once"}));

}  // namespace
}  // namespace vela::compiler
