#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(GenericTest, SpecializesFunctionsClassesAliasesAndCapturedPrimitives) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
type Id = int;
type Callback<T> = (T) => T;
type Values<T> = T[];
func identity<T>(value: T): T { return value; }
func repeat<T>(value: T, count: int): T {
  if (count == 0) { return value; }
  return repeat<T>(value, count - 1);
}
func capture<T>(value: T): () => T { return (): T => value; }
class Box<T> {
  value: T;
  get(): T { return this.value; }
}
func main(): int {
  const operation: Callback<int> = identity<Id>;
  const box: Box<Id> = Box<int>(value: operation(40));
  const copy: Box<int> = box;
  const values: Values<int> = [copy.get(), repeat<int>(2, 3)];
  const getter: () => double = capture<double>(1.5);
  if (getter() == identity<double>(1.5)) {
    return values[0] + values[1];
  }
  return 0;
}
)",
                                      isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ": " << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  EXPECT_TRUE(compiled.program.FindFunction("identity<int>").IsValid());
  EXPECT_FALSE(compiled.program.FindFunction("identity<Id>").IsValid());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

class GenericErrorTest : public testing::TestWithParam<std::string_view> {};

TEST(GenericTest, PreservesLoopBindingsAndAccessorFlagsWhenCloningTemplates) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
func identity<T>(value: T): T { return value; }
func collect<T>(values: T[]): (() => T)[] {
  const result: (() => T)[] = [];
  for (const identity in identity<T[]>(values)) {
    result.push((): T => identity);
  }
  return result;
}
func pairs<K, V>(values: Map<K, V>): (() => int)[] {
  const result: (() => int)[] = [];
  for (const [key, value] in values) {
    result.push((): int => key.length + value);
  }
  return result;
}
class Box<T> {
  stored: T;
  get value(): T { return this.stored; }
  set value(next: T): void { this.stored = next; }
}
func main(): int {
  const values = collect<int>([19, 20]);
  const map = Map<string, int>();
  map.set('ab', 1);
  const callbacks = pairs<string, int>(map);
  const box = Box<int>(stored: 0);
  box.value = values[0]() + values[1]();
  return box.value + callbacks[0]();
}
)",
                                      isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ": " << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST_P(GenericErrorTest, RejectsInvalidInstantiationAndExpandingRecursion) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StaticArguments,
    GenericErrorTest,
    testing::Values(
        "func id<T>(x: T): T { return x; } func main(): int { return id<int, double>(1); }",
        "func id<T>(x: T): T { return x; } func main(): int { return id<int>(1.0); }",
        "type Cycle<T> = Cycle<T>; func main(): void { let x: Cycle<int> = 1; }",
        "func main(): void { let value: Unknown<int> = 1; }",
        "func id<T>(x:T):T{return x;} func main():int { const id = (x:int):int => x; return "
        "id<int>(42); }",
        "func id<T,T>(x:T):T{return x;} func main():void {}",
        "func expand<T>(x: T): int { return expand<T[]>([x]); } func main(): int { return "
        "expand<int>(1); }"));

TEST(GenericTest, ConstructsGenericAliasesAndKeepsTemplateBodiesInTheirDeclarationScope) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
func identity<T>(value: T): T { return value; }
func forward<T>(value: T): T { return identity<T>(value); }
class Box<T> { value: T; }
type Alias<T> = Box<T>;
func main(): int {
  const identity: int = 0;
  return Alias<int>(value: forward<int>(42)).value;
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(GenericTest, DiagnosesDeepTypeSyntaxBeforeRecursiveDestructionCanOverflow) {
  constexpr int kExcessiveNesting = 1000;
  std::string type = "int";
  for (int index = 0; index < kExcessiveNesting; ++index) {
    type += "[]";
  }
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource("func main(value: " + type + "): void {}", isolate).Succeeded());
}

TEST(GenericTest, FusedInstantiationPreservesMatchCatchFinallyAndLambdaScopes) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
func identity<T>(value: T): T { return value; }
class Failure<T> { value: T; }
func inspect<T>(value: T, flag: bool): T {
  let retained: T = value;
  try {
    const identity = (): T => value;
    retained = identity();
  } catch (identity: Failure<T>) {
    retained = identity.value;
  } finally {
    const identity = (): T => retained;
    retained = identity();
  }
  const selected = match (flag) {
    true => retained,
    let identity => identity ? retained : value,
  };
  return identity<T>(selected);
}
func main(): int { return inspect<int>(20, true) + inspect<int>(22, false); }
)",
                                      isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(GenericTest, FusedClassInstantiationPreservesGenericBasesConstructorsAndOverrides) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
func identity<T>(value: T): T { return value; }
class Base<T> { value: T; read(): T { return this.value; } }
class Derived<T> extends Base<T> {
  extra: T;
  constructor { super(value: this.extra); }
  override read(): T { return identity<T>(this.extra); }
}
func main(): int {
  const derived = Derived<int>(extra: 21);
  return derived.read() + derived.value;
}
)",
                                      isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(GenericTest, FusedInstantiationRejectsTypeArgumentsOnShadowingBindings) {
  for (const auto source : {
           R"(
func identity<T>(value: T): T { return value; }
func apply<T>(identity: (T) => T, value: T): T { return identity<T>(value); }
func main(): int { return apply<int>(identity<int>, 42); }
)",
           R"(
func identity<T>(value: T): T { return value; }
class Failure<T> { value: T; }
func apply<T>(value: T): T {
  try { return value; }
  catch (identity: Failure<T>) { return identity<T>(value); }
}
func main(): int { return apply<int>(42); }
)",
           R"(
func identity<T>(value: T): T { return value; }
func apply<T>(value: T): T {
  return match (value) { let identity => identity<T>(value), };
}
func main(): int { return apply<int>(42); }
)",
           R"(
func identity<T>(value: T): T { return value; }
func apply<T>(values: Map<string, T>): T {
  for (const [key, identity] in values) { return identity<T>(identity); }
  return values.values()[0];
}
func main(): int { return apply<int>(Map<string, int>()); }
)",
       }) {
    SCOPED_TRACE(source);
    runtime::Isolate isolate;
    const auto compiled = CompileSource(source, isolate);
    EXPECT_FALSE(compiled.Succeeded());
    ASSERT_FALSE(compiled.diagnostics.empty());
    EXPECT_TRUE(compiled.diagnostics.front().message.contains(
        "type arguments cannot be applied to a local binding"));
  }
}

}  // namespace
}  // namespace vela::compiler
