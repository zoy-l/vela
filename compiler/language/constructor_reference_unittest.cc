#include <string_view>

#include <gtest/gtest.h>

#include "compiler/test_support/execution_expectations.h"

namespace vela::compiler {
namespace {

class ConstructorReferenceTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ConstructorReferenceTest, InvokesStronglyTypedFactoriesUnderGcPressure) {
  test::ExpectRunAnswer(GetParam());
}

TEST_P(ConstructorReferenceTest, RestoresFactoriesWithoutCompilerMetadata) {
  test::ExpectRestoredAnswer(GetParam());
}

INSTANTIATE_TEST_SUITE_P(ClassesAndContexts,
                         ConstructorReferenceTest,
                         testing::Values(
                             R"(
class Counter { value: int = 42; }
func build<T>(create: () => T): T { return create(); }
func main(): int { return build(Counter).value; }
)",
                             R"(
class Box<T> { value: T? = null; }
func build<T>(create: () => T): T { return create(); }
func main(): int { return build(Box<int>).value == null ? 42 : 0; }
)",
                             R"(
class Counter { value: int = 42; }
func build(factory: () => Counter): Counter { return factory(); }
func main(): int { return build(Counter).value; }
)",
                             R"(
class Base<P> { value: P; }
class Counter extends Base<int> {}
func build<P, C extends Base<P>>(value: P, factory: (P) => C): C {
  return factory(value);
}
func main(): int { return build(value: 42, factory: Counter).value; }
)",
                             R"(
class Base<P> { value: P; }
// 未显式应用的泛型构造值需要从工厂上下文反推 T；非泛型 Counter 无此推断步骤。
class Counter<T> extends Base<T> {}
func build<P, C extends Base<P>>(value: P, factory: (P) => C): C {
  return factory(value);
}
func main(): int { return build(value: 42, factory: Counter).value; }
)",
                             R"(
class Base {
  value: int;
  constructor(value: int = 42) { this.value = value; }
}
class Counter extends Base {}
func main(): int { const create: () => Counter = Counter; return create().value; }
)",
                             R"(
class Counter { value: int; }
func main(): int { const create: (int) => Counter = Counter; return create(42).value; }
)",
                             R"(
class Counter {
  value: int;
  constructor(value: int = 42) { this.value = value; }
}
func main(): int { const create: () => Counter = Counter; return create().value; }
)",
                             R"(
class Box<T> { value: T; }
func main(): int { const create: (int) => Box<int> = Box; return create(42).value; }
)",
                             R"(
class Box<T> { value: T; }
func construct<T>(create: (T) => Box<T>, value: T): Box<T> { return create(value); }
func main(): int { return construct<int>(Box<int>, 42).value; }
)",
                             R"(
class Base { read(): int { return 0; } }
class Child extends Base { override read(): int { return 42; } }
func main(): int { const create: () => Base = Child; return create().read(); }
)",
                             R"(
class Counter {
  value: int;
  private constructor(value: int) { this.value = value; }
  static factory(): (int) => Counter { return Counter; }
}
func main(): int { const create = Counter.factory(); return create(42).value; }
)",
                             R"(
class Counter { value: int = 42; }
type FactoryResult = Counter;
func main(): int { const create: () => Counter = FactoryResult; return create().value; }
)"));

class InvalidConstructorReferenceTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InvalidConstructorReferenceTest, RejectsInaccessibleOrIncompatibleFactories) {
  test::ExpectRejectedCheck(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    VisibilityAndTypes,
    InvalidConstructorReferenceTest,
    testing::Values("class Counter { private constructor() {} } "
                    "func main(): void { const create: () => Counter = Counter; }",
                    "class Counter { value: int; } "
                    "func main(): void { const create: () => Counter = Counter; }",
                    "class Counter { value: int; } "
                    "func main(): void { const create: (string) => Counter = Counter; }",
                    "class Counter {} "
                    "func main(): void { const create: () => int = Counter; }",
                    "class Counter {} type Nullable = Counter?; "
                    "func main(): void { const create: () => Counter = Nullable; }"));

}  // namespace
}  // namespace vela::compiler
