#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/program_test_support.h"
#include "runtime/isolate.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(TypeOperationTest, TestsAndNarrowsInterfacesWithoutChangingObjectIdentity) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(R"(
interface Measurable { measure(): int; }
interface Named { name(): string; }
class Rectangle implements Measurable, Named {
  width: int;
  height: int;
  measure(): int { return this.width * this.height; }
  name(): string { return "rectangle"; }
}
class Other implements Measurable { measure(): int { return 0; } }
func narrow(value: Measurable?): int {
  if (!(value is Rectangle)) { return 0; }
  const rectangle: Rectangle = value as Rectangle;
  const named: Named = rectangle as Named;
  if (named.name() != "rectangle") { return 0; }
  if (value is Other) { return 0; }
  return value.width * rectangle.height;
}
func main(): int {
  const rectangle = Rectangle(width: 6, height: 7);
  return narrow(rectangle);
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(TypeOperationTest, TestsNullableArraysAndScalars) {
  runtime::Isolate isolate;
  const auto entry = test::CompileEntry(R"(
func main(): bool {
  const value: int? = 42;
  const array: int[]? = [42];
  const absent: string? = null;
  return value is int && array is int[] && absent is string? &&
         !(absent is string) && (value as int) == 42 && (array as int[])[0] == 42;
}
)",
                                        isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = vm::VirtualMachine(isolate).Execute(*entry);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_TRUE(result.value.AsBoolean());
}

TEST(TypeOperationTest, ReportsCheckedCastFailure) {
  runtime::Isolate isolate;
  const auto entry = test::CompileEntry(R"(
interface I {}
class A implements I {}
class B implements I {}
func require(value: I): B { return value as B; }
func main(): B { return require(A()); }
)",
                                        isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = vm::VirtualMachine(isolate).Execute(*entry);
  EXPECT_EQ(result.status, vm::ExecutionStatus::InvalidCast);
}

class TypeOperationErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(TypeOperationErrorTest, RejectsConversionsAndInvalidatedRefinements) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(StrictTypes,
                         TypeOperationErrorTest,
                         testing::Values("func main(): double { return 42 as double; }",
                                         "func main(): int { return 42.5 as int; }",
                                         R"(
interface I {}
class A implements I { value: int; }
class B implements I {}
func main(): int {
  let value: I = A(value: 42);
  const change = (): void => { value = B(); };
  if (value is A) { change(); return value.value; }
  return 0;
}
)"));

}  // namespace
}  // namespace vela::compiler
