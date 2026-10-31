#include <array>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/bytecode/compiler.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/test_support/program_test_support.h"
#include "compiler/test_support/test_sources.h"
#include "runtime/isolate.h"

namespace vela::compiler {
namespace {

TEST(GenericMethodTest, ExecutesOwnerMethodAndInterfaceApplications) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(test::kGenericMethodsSource, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, RestoresConcreteSelectorsAndCapturedReceiversFromImage) {
  runtime::Isolate isolate(1);
  const auto compiled = test::ReloadSource(test::kGenericMethodsSource, isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, RestoresAsyncMethodFramesAndTypedExceptionsFromImage) {
  runtime::Isolate isolate(1);
  const auto compiled = test::ReloadSource(test::kGenericAsyncMethodsSource, isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteFutureInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, UsesQualifiedBoundsAndMethodApplicationsAcrossModules) {
  const std::array inputs{
      frontend::SourceInput{
          .path = "library.vela",
          .text = R"(
export interface Reader<T> { read(): T; }
export class Box<T> implements Reader<T> {
  value: T;
  read(): T { return this.value; }
  apply<R extends Reader<U>, U>(reader: R, operation: (T, U) => T): T {
    return operation(this.value, reader.read());
  }
}
)",
      },
      frontend::SourceInput{
          .path = "main.vela",
          .text = R"(
import 'library.vela' as lib;
func main(): int {
  const box = lib.Box<int>(value: 20);
  return box.apply(lib.Box<int>(value: 22), (left, right) => left + right);
}
)",
      },
  };
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanModules(inputs, "main.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, PreservesInheritanceSuperValuesAndParameterShadowing) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
class Base<T> {
  identity<T>(value: T): T { return value; }
  produce<U>(value: U): U { return value; }
}
class Derived<T> extends Base<T> {
  override identity<U>(value: U): U { return super.identity<U>(value); }
  read(): int {
    const operation = super.produce<int>;
    return operation(20);
  }
}
func main(): int {
  const owner = Derived<string>();
  const operation = owner.identity<int>;
  return owner.read() + operation(22);
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, InfersResultsFromContextWithoutInputEvidence) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
class Factory {
  create<T>(): T[] { return []; }
  identity<T>(value: T): T { return value; }
}
func main(): int {
  const factory = Factory();
  const values: int[] = factory.create();
  const operation: (int) => int = factory.identity;
  return operation(42) + values.length;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, DefersMethodValuesAndNestedCallsUntilCallbackContextIsKnown) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
class Operations {
  identity<T>(value: T): T { return value; }
  create<T>(): T[] { return []; }
  static copy<T>(value: T): T { return value; }
}
let calls: int = 0;
func make(): Operations { calls += 1; return Operations(); }
func apply<T>(operation: (T) => T, value: T): T { return operation(value); }
func choose<T>(items: T[], value: T): T { return value; }
func main(): int {
  const owner = Operations();
  const first = apply(owner.identity, 10);
  const second = apply(make().identity, 10);
  const third = apply(Operations.copy, 10);
  const fourth = apply(true ? owner.identity : owner.identity, 10);
  const fifth = apply(true ? make().identity : make().identity, 0);
  const values = [0].map(owner.identity);
  return first + second + third + choose(owner.create(), fourth) + fifth + values[0] + calls;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, DefersWrappedMethodValuesAndPreservesMatchBindingIdentity) {
  runtime::Isolate isolate(1);
  const auto compiled = test::ReloadSource(test::kWrappedGenericMethodValuesSource, isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, PrefixCallbacksInferThroughConditionalAndCoalescingValues) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
class Operations { identity<T>(value: T): T { return value; } }
let calls: int = 0;
func make(): Operations { calls += 1; return Operations(); }
func main(): int {
  const absent: ((int) => int)? = null;
  const first = [20].map(true ? make().identity : make().identity)[0];
  const second = [20].map(absent ?? make().identity)[0];
  return first + second + calls;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, InterfaceApplicationsMaterializeImplementationsRegisteredLater) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
interface Mapper { identity<T>(value: T): T; }
class Implementation<U> implements Mapper {
  identity<T>(value: T): T { return value; }
}
func apply(mapper: Mapper): int { return mapper.identity(21); }
func main(): int { return apply(Implementation<int>()) + apply(Implementation<string>()); }
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GenericMethodTest, RejectsTypeArgumentsWithoutAStaticGenericDeclaration) {
  for (const auto source : {
           std::string_view(R"(
class Owner { method(): int { return 42; } }
class Derived extends Owner { read(): int { return super.method<int>(); } }
func main(): int { return Derived().read(); }
)"),
           std::string_view(R"(
class Owner { identity<T>(value: T): T { return value; } }
func main(): int { const owner: dynamic = Owner(); return owner.identity<int>(42); }
)"),
       }) {
    SCOPED_TRACE(source);
    runtime::Isolate isolate;
    const auto compiled = CompileSource(source, isolate);
    EXPECT_FALSE(compiled.Succeeded());
    EXPECT_FALSE(compiled.diagnostics.empty());
  }
}

}  // namespace
}  // namespace vela::compiler
