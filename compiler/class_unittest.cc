#include <algorithm>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class ClassConstructionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ClassConstructionTest, InitializesFieldsAndExecutesConstructorUnderGcPressure) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(GetParam(), isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ": " << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(FieldParameters,
                         ClassConstructionTest,
                         testing::Values(
                             R"(
class Counter {
  public param value: int;
  public param step: int = 1;
  private count: int = 0;
  constructor { this.count = this.count + 1; }
  public next(): int { this.value = this.value + this.step; return this.value + this.count; }
}
func main(): int { const c = Counter(value: 40); return c.next(); }
)",
                             R"(
func defaultStep(): int { return 2; }
class Counter {
  private param value: int;
  public param step: int = defaultStep();
  public next(): int { return this.value + this.step; }
}
func main(): int {
  const defaultStep = (): int => 99;
  return Counter(value: 40).next();
}
)",
                             R"(
class Pair {
  public value: int;
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
  private param value: T;
  public get(): T { return this.value; }
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
  public static next(): int { Counter.value = Counter.value + 1; return Counter.value; }
}
func main(): int { const f: () => int = Counter.next; f(); return f(); }
)",
                             R"(
class Callback {
  public static operation: (int) => int = (value: int): int => value + 2;
}
func main(): int { return Callback.operation(40); }
)",
                             R"(
class Value {
  value: int;
  constructor() { if (true) { this.value = 42; } }
}
func main(): int { return Value().value; }
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
        ClassError{"class A { param x: int; constructor(x: int) { this.x = x; } }",
                   "mutually exclusive"},
        ClassError{"class A { param x: int; } func main(): A { return A(1); }", "must be named"},
        ClassError{"class A { x: int; }", "requires an initializer"},
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
        ClassError{"class A { private param x: int; } func main(): int { return A(x: 1).x; }",
                   "private member"},
        ClassError{
            "class A { private get(): int { return 1; } } func main(): int { return A().get(); }",
            "private member"},
        ClassError{"class A { private constructor() {} } func main(): A { return A(); }",
                   "private member"},
        ClassError{"class A { static param x: int = 0; }", "cannot use param"},
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
  const auto copy = compiled.program;
  vm::VirtualMachine machine(isolate);
  const auto first = machine.Execute(copy.FindFunction("main"));
  ASSERT_TRUE(first.Succeeded());
  EXPECT_EQ(first.value.AsInteger(), 41);
  isolate.heap().Collect();
  const auto second = machine.Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(second.Succeeded());
  EXPECT_EQ(second.value.AsInteger(), 42);
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
  const auto compiled = CompileSource(R"(
class Trace {
  static value: int = 0;
  static mark(value: int): int { Trace.value = Trace.value * 10 + value; return value; }
}
class Target {
  public param first: int = Trace.mark(9);
  public param second: int = Trace.mark(8);
  private tail: int = Trace.mark(3);
  constructor { Trace.mark(4); }
}
func main(): int {
  const target = Target(second: Trace.mark(2), first: Trace.mark(1));
  return Trace.value;
}
)",
                                      isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ": " << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 2134);
}

}  // namespace
}  // namespace vela::compiler
