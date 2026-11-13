#include "compiler/aot/native_compiler.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/native/code_generator.h"
#include "compiler/test_support/native_pass_test_support.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::aot {
namespace {

constexpr unsigned kRepeatedRounds = 10;
constexpr unsigned kSecondTraversalRounds = 6;

unsigned AdditionChainLength(const llvm::Value& value) {
  const auto* operation = llvm::dyn_cast<llvm::BinaryOperator>(&value);
  if (operation == nullptr || operation->getOpcode() != llvm::Instruction::FAdd) {
    return 0;
  }
  EXPECT_FALSE(operation->getFastMathFlags().any());
  return 1 + std::max(AdditionChainLength(*operation->getOperand(0)),
                      AdditionChainLength(*operation->getOperand(1)));
}

void ExpectRepeatedTraversals(std::string_view source, std::initializer_list<unsigned> counts) {
  const auto prepared = ir::test::Prepare(source);
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto result = Compile(prepared.program, {.exports = {"update"}, .preserve_ir = true});
  ASSERT_TRUE(result) << frontend::DiagnosticsText(result.error());
  llvm::LLVMContext context;
  const auto module = native::test::ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  std::vector<unsigned> chains;
  native::test::WithFunctionLoops(*module, [&](llvm::Function&, const llvm::LoopInfo& loops) {
    for (const auto* loop : loops.getLoopsInPreorder()) {
      EXPECT_TRUE(loop->getSubLoops().empty()) << result->llvm_ir;
      for (const auto* block : loop->blocks()) {
        for (const auto& instruction : *block) {
          if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
            chains.push_back(AdditionChainLength(*store->getValueOperand()));
          }
        }
      }
    }
  });
  // 仅统计加法总数会把热路径的十次单独遍历误认成成功；要求单次存储前保留完整加法链。
  for (const auto count : counts) {
    EXPECT_NE(std::ranges::find(chains, count), chains.end()) << count << '\n' << result->llvm_ir;
  }
}

TEST(RepeatedFieldTest, FoldsParameterTraversalsWithoutReassociatingAdds) {
  ExpectRepeatedTraversals(R"(
export class Point { x: double; dx: double; }
export func update(points: Point[]): void {
  for (let round = 0; round < 10; round++) {
    for (const point in points) { point.x += point.dx; }
  }
}
)",
                           {kRepeatedRounds});
}

TEST(RepeatedFieldTest, FoldsWarmGlobalTraversalsAndPeelsInitialization) {
  ExpectRepeatedTraversals(R"(
class Point { x: double; y: double; z: double; dx: double; dy: double; dz: double; }
let points: Point[] = [];
export func update(): void {
  for (let round = 0; round < 10; round++) {
    for (const point in points) {
      point.x += point.dx; point.y += point.dy; point.z += point.dz;
    }
  }
}
)",
                           {kRepeatedRounds - 1, kRepeatedRounds});
}

TEST(RepeatedFieldTest, FoldsEveryIndependentTraversalInTheFunction) {
  ExpectRepeatedTraversals(
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
)",
      {kSecondTraversalRounds - 1, kSecondTraversalRounds, kRepeatedRounds - 1, kRepeatedRounds});
}

class RepeatedFieldBoundaryTest : public testing::TestWithParam<unsigned> {};

TEST_P(RepeatedFieldBoundaryTest, FoldsTheMinimumAndMaximumSupportedTripCounts) {
  const auto count = GetParam();
  const auto source =
      "export class Point { x: double; dx: double; } "
      "export func update(points: Point[]): void { for (let round = 0; round < " +
      std::to_string(count) +
      "; round++) { for (const point in points) { point.x += point.dx; } } }";
  ExpectRepeatedTraversals(source, {count});
}

INSTANTIATE_TEST_SUITE_P(Boundaries, RepeatedFieldBoundaryTest, testing::Values(2U, 32U));

}  // namespace
}  // namespace vela::compiler::aot
