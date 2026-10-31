#include <algorithm>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/bytecode/compiler.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class ClassConstructionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ClassConstructionTest, InitializesFieldsAndExecutesConstructorUnderGcPressure) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(GetParam(), isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

INSTANTIATE_TEST_SUITE_P(GeneratedAndExplicitConstructors,
                         ClassConstructionTest,
                         testing::Values(
                             R"(
class Counter {
  value: int;
  step: int = 1;
  private count: int = 0;
  constructor { this.count = this.count + 1; }
  next(): int { this.value = this.value + this.step; return this.value + this.count; }
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
  private label: string = "a" + "b";
  constructor(value: int, positive: bool) {
    if (positive) { this.value = value; } else { this.value = -value; }
  }
  read(): int { return this.value + this.label.length; }
}
func main(): int { return Pair(40, true).read(); }
)",
                             R"(
class Box<T> {
  private value: T;
  constructor(value: T) { this.value = value; }
  get(): T { return this.value; }
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
  private static value: int = 40;
  static next(): int { Counter.value = Counter.value + 1; return Counter.value; }
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
  private adjustment: int = 2;
  constructor(initial: int) { this.value = initial + this.adjustment; }
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
class Box { public: int; param: int; }
func main(): int { return Box(public: 40, param: 2).public + 2; }
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
    super(value: value);
    this.bonus = bonus;
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

struct ClassError final {
  std::string_view source;
  std::string_view message;
};

class ClassContractErrorTest : public testing::TestWithParam<ClassError> {};

TEST_P(ClassContractErrorTest, DiagnosesTheViolatedContract) {
  runtime::Isolate isolate;
  const auto& parameter = GetParam();
  const auto compiled = CompileSource(parameter.source, isolate);
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [&](const auto& diagnostic) -> bool {
    return diagnostic.message.contains(parameter.message);
  }));
}

INSTANTIATE_TEST_SUITE_P(
    ConstructionAndVisibility,
    ClassContractErrorTest,
    testing::Values(
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
        ClassError{"class A { value: int; } class B extends A { bonus: int; }", "must call super"},
        ClassError{"class A { value: int; } class B extends A { constructor {} }",
                   "must call super"},
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
        ClassError{"class A { private x: int; }", "requires an initializer"},
        ClassError{"class A { private x: int = 0; } func main(): A { return A(x: 42); }",
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
        ClassError{"class A { private x: int = 1; } func main(): int { return A().x; }",
                   "private member"},
        ClassError{
            "class A { private get(): int { return 1; } } func main(): int { return A().get(); }",
            "private member"},
        ClassError{"class A { private constructor() {} } func main(): A { return A(); }",
                   "private member"},
        ClassError{"class A { static x: int = 0; } func main(): A { return A(x: 1); }",
                   "unknown named argument 'x'"},
        ClassError{"class A { static read(): int { return this.x; } }", "unknown name"},
        ClassError{
            "class A { static read(): int { return 1; } } func main(): int { return A().read(); }",
            "through its class"},
        ClassError{"class A { read(): int { return 1; } } func main(): int { return A.read(); }",
                   "static member"},
        ClassError{"class A { private static x: int = 1; } func main(): int { return A.x; }",
                   "private member"},
        ClassError{
            "class A { f(): int { return 1; } } func main(): void { A().f = (): int => 42; }",
            "immutable"},
        ClassError{
            "class A { static f(): int { return 1; } } func main(): void { A.f = (): int => 42; }",
            "immutable"},
        ClassError{"enum E { a,b } func main(): void { E.a = E.b; }", "immutable"}));

TEST(StaticFieldTest, RetainsRootedStateAcrossExecutionsAndProgramCopies) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
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
  const auto results = test::ExecuteProgramCopy(compiled.program, isolate);
  ASSERT_TRUE(results) << results.error();
  EXPECT_EQ(results->front(), 41);
  EXPECT_EQ(results->back(), 42);
}

TEST(StaticFieldTest, DiagnosesInitializationCyclesAndRetriesAfterFailure) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
class Cycle { static value: int = Cycle.other; static other: int = Cycle.value; }
func main(): int { return Cycle.value; }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  vm::VirtualMachine machine(isolate);
  const auto entry = compiled.program.FindFunction("main");
  EXPECT_EQ(machine.Execute(entry).status, vm::ExecutionStatus::CyclicInitialization);
  EXPECT_EQ(machine.Execute(entry).status, vm::ExecutionStatus::CyclicInitialization);
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
  private tail: int = Trace.mark(3);
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
  const auto result = test::RunInteger(R"(
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
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(ClassInitializationTest, DefaultsUseDeclarationScopeBeforeConstructorParameters) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
let seed = 40;
class Generated { value: int = seed; }
class Explicit { value: int = seed; constructor(seed: int) { this.value += seed; } }
func main(): int {
  let seed = 999;
  return Generated().value + Explicit(seed: 2).value - 40;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
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
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

}  // namespace
}  // namespace vela::compiler
