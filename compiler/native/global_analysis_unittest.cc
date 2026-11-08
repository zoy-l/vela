#include "compiler/native/global_analysis.h"

#include <algorithm>
#include <cstddef>

#include <gtest/gtest.h>

#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::native {
namespace {

TEST(GlobalAnalysisTest, RecursiveNormalReturnsInheritTheBaseCaseInitialization) {
  const auto prepared = ir::test::Prepare(R"(
let low = 0;
let high = 0;
let untouched = 0;
func split(depth: int): void {
  if (depth == 0) { low = 1; high = 2; return; }
  if (depth > 0) { split(depth - 1); low++; }
  else { split(depth + 1); high++; }
}
export func calculate(depth: int): int { split(depth); return low + high + untouched; }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto split = std::ranges::find(program.functions, "split", &ir::Function::name);
  ASSERT_NE(split, program.functions.end());
  const auto facts = AnalyzeGlobalAccess(program, NativeCodePlacement::Linked);
  const auto index = static_cast<std::size_t>(split - program.functions.begin());
  const auto& initialized = facts.initialized_on_return[index];
  ASSERT_EQ(initialized.size(), 2U);
  for (const auto slot : initialized) {
    EXPECT_EQ(facts.readonly_slots[slot], 0);
  }
  EXPECT_EQ(std::ranges::count(facts.readonly_slots, 1), 1);
  const auto& instructions = split->instructions;
  const auto& accesses = facts.initialized_accesses[index];
  std::size_t reads = 0;
  std::size_t stores = 0;
  for (std::size_t position = 0; position < instructions.size(); ++position) {
    if (accesses[position] == 0) {
      continue;
    }
    if (instructions[position].opcode == Opcode::GetGlobal) {
      ++reads;
    }
    if (instructions[position].opcode == Opcode::SetGlobal) {
      ++stores;
    }
  }
  EXPECT_EQ(reads, 2U);
  EXPECT_EQ(stores, 2U);
}

TEST(GlobalAnalysisTest, EarlyReturnsAndZeroIterationLoopsDoNotEstablishReadiness) {
  const auto prepared = ir::test::Prepare(R"(
let value = 0;
func partial(choose: bool): void { if (choose) { return; } value = 1; }
func loop(count: int): void { for (let index = 0; index < count; index++) { value = index; } }
func recursive(depth: int): void { if (depth == 0) { return; } recursive(depth - 1); value = 2; }
export func calculate(count: int, choose: bool): int {
  partial(choose); loop(count); recursive(count); return value;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto facts = AnalyzeGlobalAccess(program, NativeCodePlacement::Linked);
  for (const auto& function : program.functions) {
    if (function.name == "partial" || function.name == "loop" || function.name == "recursive") {
      const auto index = static_cast<std::size_t>(&function - program.functions.data());
      EXPECT_TRUE(facts.initialized_on_return[index].empty()) << function.name;
    }
  }
}

TEST(GlobalAnalysisTest, ReadsAndMutualRecursionPropagateOnlyAcrossSuccessfulCalls) {
  const auto prepared = ir::test::Prepare(R"(
let value = 42;
external func input(): int;
func first(depth: int): int { if (depth == 0) { return value; } return second(depth - 1); }
func second(depth: int): int { return first(depth); }
func guarded(): int { try { return value; } catch (error: Error) { return 0; } }
async func deferred(): Future<int> { return value; }
export func calculate(depth: int): int { return first(depth) + second(depth) + input(); }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto facts = AnalyzeGlobalAccess(program, NativeCodePlacement::Linked);
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions[index];
    const auto& initialized = facts.initialized_on_return[index];
    if (function.name == "first" || function.name == "second" || function.name == "calculate") {
      EXPECT_EQ(initialized.size(), 1U) << function.name;
    } else if (function.name == "guarded" || function.name == "deferred" ||
               !function.native_symbol.empty()) {
      EXPECT_TRUE(initialized.empty()) << function.name;
    }
  }
}

}  // namespace
}  // namespace vela::compiler::native
