#include "compiler/aot/native_compiler.h"
#include "runtime/native/native_symbols.h"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/test_support/native_pass_test_support.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::aot {
namespace {

void ExpectInterruptibleTraversals(std::string_view source) {
  const auto prepared = ir::test::Prepare(source);
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto result = Compile(prepared.program, {.exports = {"update"}, .preserve_ir = true});
  ASSERT_TRUE(result) << frontend::DiagnosticsText(result.error());
  llvm::LLVMContext context;
  const auto module = native::test::ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  // 控制请求能观察遍历的已完成前缀；不能再要求把所有轮次折叠为一个不可观察的字段链。
  EXPECT_GT(native::test::CountLoopsCalling(*module, runtime::kNativeSafepointSymbol), 0U);
  EXPECT_EQ(result->llvm_ir.find(" reassoc "), std::string::npos);
  EXPECT_EQ(result->llvm_ir.find(" fast "), std::string::npos);
}

TEST(RepeatedFieldTest, KeepsGlobalTraversalPrefixesInterruptible) {
  ExpectInterruptibleTraversals(R"(
class Point { x: double; y: double; z: double; dx: double; dy: double; dz: double; }
let points: Point[] = [];
export func update(): void {
  for (let round = 0; round < 10; round++) {
    for (const point in points) {
      point.x += point.dx; point.y += point.dy; point.z += point.dz;
    }
  }
}
)");
}

TEST(RepeatedFieldTest, KeepsIndependentTraversalsInterruptible) {
  ExpectInterruptibleTraversals(
      R"(
class Point { x: double; dx: double; }
let first: Point[] = [];
let second: Point[] = [];
export func update(): void {
  for (let round = 0; round < 10; round++) {
    for (const point in first) { point.x += point.dx; }
  }
  for (let round = 0; round < 6; round++) {
    for (const point in second) { point.x += point.dx; }
  }
}
)");
}

class RepeatedFieldTripCountTest : public testing::TestWithParam<unsigned> {};

TEST_P(RepeatedFieldTripCountTest, KeepsParameterTraversalPrefixesInterruptible) {
  const auto count = GetParam();
  const auto source =
      "export class Point { x: double; dx: double; } "
      "export func update(points: Point[]): void { for (let round = 0; round < " +
      std::to_string(count) +
      "; round++) { for (const point in points) { point.x += point.dx; } } }";
  ExpectInterruptibleTraversals(source);
}

INSTANTIATE_TEST_SUITE_P(TripCounts, RepeatedFieldTripCountTest, testing::Values(2U, 10U, 32U));

}  // namespace
}  // namespace vela::compiler::aot
