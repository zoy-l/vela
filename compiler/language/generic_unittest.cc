#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>

#include "gtest/gtest.h"

#include "compiler/ir/preparation_test_support.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_program_state.h"

namespace vela::compiler {
namespace {

TEST(GenericTest, SpecializesFunctionsClassesAliasesAndCapturedPrimitives) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(R"(
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
  ASSERT_TRUE(compiled) << compiled.error();
  EXPECT_TRUE((*compiled)->FindFunction("identity<int>").has_value());
  EXPECT_FALSE((*compiled)->FindFunction("identity<Id>").has_value());
  const auto result = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

class GenericErrorTest : public testing::TestWithParam<std::string_view> {};

TEST(GenericTest, ExecutesBoundedDefinitionsWithConcreteInterfaceApplications) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
interface Comparable<T> { compareTo(other: T): int; }
class Score implements Comparable<Score> {
  value: int;
  compareTo(other: Score): int { return this.value - other.value; }
}
func compare<T extends Comparable<T>>(left: T, right: T): int {
  return left.compareTo(right);
}
func main(): int {
  const left = Score(value: 44);
  const right = Score(value: 2);
  const operation: (Score, Score) => int = compare;
  return operation(left, right);
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, InfersPayloadsAndUsesTheProvenFBoundParentSpecialization) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileCleanSource(R"(
interface Reader<E> { read(): E; }
class IntReader implements Reader<int> { read(): int { return 20; } }
class A<T> { read(): int { return 20; } }
class B extends A<B> {}
class C extends B {}
func read<X extends A<X>>(value: X): int { return value.read(); }
func map<T extends Reader<E>, E, U>(operation: (E) => U, source: T): U {
  return operation(source.read());
}
func main(): int { return read(C()) + map(value => value + 2, IntReader()); }
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  EXPECT_TRUE((*compiled)->FindFunction("read<B>").has_value());
  EXPECT_FALSE((*compiled)->FindFunction("read<C>").has_value());
  const auto result = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, BoundEvidenceDoesNotRepeatArgumentsOrDeferTheirRuntimeEffects) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
interface Reader<E> { read(): E; }
class IntReader implements Reader<int> { read(): int { return 41; } }
let calls: int = 0;
func make(): IntReader { calls += 1; return IntReader(); }
func apply<T extends Reader<E>, E, U>(operation: (E) => U, source: T): U {
  return operation(source.read());
}
func main(): int {
  const value = apply(item => item + calls, make());
  return value + (calls - 1) * 100;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, NestedOutputVariablesWaitForOuterArgumentEvidence) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
func create<T, U>(value: T): U[] { return []; }
func maybe<T, U>(value: T): U[]? { return null; }
func identity<T>(value: T): T { return value; }
func choose<T>(items: T[], value: T): T { return value; }
class Box<T> { value: T; }
func main(): int {
  const first = choose(create(1), 20);
  const second = choose(identity(create('input')), 22);
  const boxes = choose(create(false), Box(value: first + second));
  const conditional = choose(true ? create(0) : [], boxes.value);
  const matched = choose(match (true) { true => create(0), false => [], }, conditional);
  return choose(maybe(0) ?? [], matched);
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, InferredAndExplicitUsesHaveTheSameTypedIR) {
  constexpr std::string_view kExplicit = R"(
func identity<T>(value: T): T { return value; }
class Box<T> { value: T; }
func main(): int {
  const operation: (int) => int = identity<int>;
  const box = Box<int>(value: identity<int>(40));
  return box.value + operation(2);
}
)";
  constexpr std::string_view kInferred = R"(
func identity<T>(value: T): T { return value; }
class Box<T> { value: T; }
func main(): int {
  const operation: (int) => int = identity;
  const box = Box(value: identity(40));
  return box.value + operation(2);
}
)";
  const auto explicit_result = ir::test::Prepare(kExplicit);
  const auto inferred_result = ir::test::Prepare(kInferred);
  ASSERT_TRUE(explicit_result.Succeeded());
  ASSERT_TRUE(inferred_result.Succeeded());
  const auto& expected = explicit_result.program.functions;
  const auto& actual = inferred_result.program.functions;
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto& left = expected.at(index);
    const auto& right = actual.at(index);
    EXPECT_EQ(left.name, right.name);
    EXPECT_EQ(std::tie(left.register_count, left.parameter_count),
              std::tie(right.register_count, right.parameter_count));
    ASSERT_EQ(left.instructions.size(), right.instructions.size());
    for (std::size_t position = 0; position < left.instructions.size(); ++position) {
      EXPECT_EQ(ir::test::InstructionOperands(left.instructions.at(position)),
                ir::test::InstructionOperands(right.instructions.at(position)));
    }
  }
}

TEST(GenericTest, PreservesNestedTypeStructureAndInfersGenericCallbackValues) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
class Box<T> { value: T; }
func identity<T>(value: T): T { return value; }
func unbox<T>(box: Box<T[]>): T[] { return box.value; }
func apply<T, U>(operation: (T) => U, value: T): U { return operation(value); }
func main(): int {
  const values = unbox(Box(value: [40, 2]));
  const copy = values.map(identity);
  const texts = values.map(42.toString);
  if (texts.join(',') != '42,42') { return 0; }
  return apply(identity, copy[0] + copy[1]);
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, InfersVoidOnlyInPositionsThatPermitVoid) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
func apply<T, U>(operation: (T) => U, value: T): U { return operation(value); }
func main(): int {
  let total = 0;
  apply(value => { total += value; }, 42);
  return total;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, InfersCallsConstructorsFunctionValuesAndRecursiveBodies) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(R"(
type Operation<T> = (T) => T;
func identity<T>(value: T): T { return value; }
func forward<T>(value: T, count: int): T {
  if (count == 0) { return value; }
  return forward(value, count - 1);
}
class Box<T> { value: T; get(): T { return this.value; } }
func unbox<T>(box: Box<T>): T { return box.get(); }
func main(): int {
  const operation: Operation<int> = identity;
  const box = Box(value: operation(40));
  return unbox(box) + forward(identity(2), 3);
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  EXPECT_TRUE((*compiled)->FindFunction("identity<int>").has_value());
  EXPECT_TRUE((*compiled)->FindFunction("forward<int>").has_value());
  const auto result = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, InfersCallbacksAfterIndependentArgumentsAndPropagatesNestedContexts) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
func apply<T, U>(operation: (T) => U, value: T): U { return operation(value); }
func identity<T>(value: T): T { return value; }
func empty<T>(): T[] { return []; }
class Box<T> { value: T; }
func make(): Box<int[]> { return Box(value: empty()); }
func main(): int {
  const result = apply(value => value + 2, 40);
  const nested: (int) => (int) => int = identity(left => right => left + right);
  const arrays: int[][] = [empty(), true ? empty() : empty()];
  return result + nested(1)(1) - 2 + make().value.length + arrays[0].length;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, SharesArrayAndOrdinaryCallbackInference) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
func apply<T, U>(operation: (T) => U, value: T): U { return operation(value); }
func main(): int {
  const items = [19, 20];
  const mapped = items.map(value => value + 1);
  const total = mapped.reduce((sum, value) => sum + value, 1);
  return apply(value => value, total);
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, InfersTransparentGenericConstructorsAndExplicitConstructorParameters) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
class Box<T> { value: T; constructor(input: T) { this.value = input; } }
type Alias<Item> = Box<Item>;
type Dictionary<Value> = Map<string, Value>;
func main(): int {
  const box = Alias(input: 40);
  const values: Dictionary<int> = Dictionary();
  values.set('answer', box.value + 2);
  return values.get('answer') ?? 0;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, DeferredInferenceKeepsEarlierArgumentsBeforeLaterMutation) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
let mutate: () => void = (): void => {};
func create<T>(): T[] { mutate(); return []; }
func choose<T>(first: T, second: T[]): T { return first; }
func main(): int {
  let value: int? = 42;
  mutate = () => { value = null; };
  if (value != null) { return choose(value, create()); }
  return 0;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, ExecutesIndependentSpecializationsBeyond256AndRestoresWideCodeFromImage) {
  constexpr std::size_t kTypes = 257;
  std::string source = "class Box<T> { value: T; } func identity<T>(value: T): T { return value; }";
  for (std::size_t index = 0; index < kTypes; ++index) {
    source += "class Tag" + std::to_string(index) + " { value: int; }";
  }
  source += "func main(): int { let total = 0; for (let round = 0; round < 2; round++) { try {";
  for (std::size_t index = 0; index < kTypes; ++index) {
    const auto type = "Tag" + std::to_string(index);
    source.append("total += identity<Box<")
        .append(type)
        .append(">>(Box<")
        .append(type)
        .append(">(value: ")
        .append(type)
        .append("(value: 1))).value.value;");
  }
  source +=
      "const zero = round - round; total += 1 / zero; } catch (error: Error) { "
      "if (error.code != 'DivisionByZero') { return -1; } } } return total; }";
  runtime::Isolate isolate;
  const auto compiled = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kTypes * 2);
  const auto image = runtime::Heap::NativeProgram(*(*compiled)->state().front().AsObject()).image_owner;
  runtime::Isolate restored;
  const auto loaded = runtime::NativeInstance::Instantiate(restored, image);
  ASSERT_TRUE(loaded) << loaded.error().message;
  const auto resumed = test::ExecuteInteger(**loaded);
  ASSERT_TRUE(resumed) << resumed.error();
  EXPECT_EQ(*resumed, kTypes * 2);
}

TEST(GenericTest, PreservesLoopBindingsAndAccessorFlagsWhenCloningTemplates) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
func identity<T>(value: T): T { return value; }
func collect<T>(values: T[]): (() => T)[] {
  const result: (() => T)[] = [];
  for (const identity in identity<T[]>(values)) {
    result.push((): T => identity);
  }
  return result;
}
func pairs<K extends string, V extends int>(values: Map<K, V>): (() => int)[] {
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
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST_P(GenericErrorTest, RejectsInvalidInstantiationAndExpandingRecursion) {
  runtime::Isolate isolate;
  EXPECT_FALSE(test::CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StaticArguments,
    GenericErrorTest,
    testing::Values(
        "let mutate:()=>void=():void=>{}; func create<T>():T[]{mutate();return[];} "
        "func choose<T>(first:T[],second:T):T{return second;} "
        "func main():int{let value:int?=42;mutate=()=>{value=null;};"
        "if(value!=null){return choose(create(),value);}return 0;}",
        "func make<T>(): T[] { return []; } func main(): void { const items = make(); }",
        "func id<T>(x:T):T{return x;} func main():void { const value = id; }",
        "func choose<T>(x:T,y:T):T{return x;} func main():void { choose(1, 2.0); }",
        "func id<T>(x:T):T{return x;} func main():void { const value: double = id(1); }",
        "func id<T>(x:T[]):T[]{return x;} func main():void { const items = [1]; "
        "const value: dynamic[] = id(items); }",
        "func expand<T>(x: T): int { return expand([x]); } func main(): int { return expand(1); }",
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
  const auto result = test::RunInteger(R"(
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
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, DiagnosesDeepTypeSyntaxBeforeRecursiveDestructionCanOverflow) {
  constexpr int kExcessiveNesting = 1000;
  std::string type = "int";
  for (int index = 0; index < kExcessiveNesting; ++index) {
    type += "[]";
  }
  runtime::Isolate isolate;
  EXPECT_FALSE(test::CompileSource("func main(value: " + type + "): void {}", isolate).Succeeded());
}

TEST(GenericTest, FusedInstantiationPreservesMatchCatchFinallyAndLambdaScopes) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
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
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, FusedClassInstantiationPreservesGenericBasesConstructorsAndOverrides) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
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
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericTest, FusedInstantiationRejectsTypeArgumentsOnShadowingBindings) {
  for (const auto* const source : {
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
    const auto compiled = test::CompileSource(source, isolate);
    EXPECT_FALSE(compiled.Succeeded());
    ASSERT_FALSE(compiled.diagnostics.empty());
    EXPECT_TRUE(compiled.diagnostics.front().message.contains(
        "type arguments cannot be applied to a local binding"));
  }
}

}  // namespace
}  // namespace vela::compiler
