#include <algorithm>
#include <string_view>

#include "base/opcode.h"
#include "gtest/gtest.h"

#include "compiler/ir/ir.h"
#include "compiler/test_support/preparation_test_support.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

class FlowExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(FlowExecutionTest, ExecutesEveryReachableExit) {
  runtime::Isolate isolate;
  const auto entry = test::CompileCleanSource(GetParam(), isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
}

INSTANTIATE_TEST_SUITE_P(
    BranchesLoopsAndBindings,
    FlowExecutionTest,
    testing::Values(
        R"(func choose(flag: bool): int { if (flag) return 1; else return 2; }
           func main(): bool { return choose(true) == 1 && choose(false) == 2; })",
        R"(func finish(flag: bool): void { if (flag) return; }
           func main(): bool { finish(true); finish(false); return true; })",
        R"(func f(flag: bool): int { while (true) { if (flag) return 1; else break; } return 2; }
           func main(): bool { return f(true) == 1 && f(false) == 2; })",
        R"(func main(): bool { let sum = 0; for (let i = 0; i < 5; i = i + 1) {
             if (i == 1) continue; if (i == 4) break; sum = sum + i;
           } return sum == 5; })",
        R"(func main(): bool { let count = 0; let sum = 0; for (;;) {
             count = count + 1; if (count < 3) continue; sum = sum + count; if (count == 4) break;
           } return sum == 7; })",
        R"(func main(): bool { let count = 0; while (true) {
             while (true) { break; } count = count + 1; if (count == 3) break;
           } return count == 3; })",
        R"(class State { value: int; bump(): bool { this.value = this.value + 1; return false; } }
           func main(): bool { const state = State(value: 0); if (state.bump() || true) {
             const values = [state.value]; values[0] = 2; state.value = values[0];
           } return state.value == 2; })",
        R"(func main(): bool { const value = 1; { let value = 2; value = 3; }
           return value == 1; })",
        R"(let calls = 0;
           func index(): int { calls += 1; return 0; }
           func main(): bool {
             const values = [40]; values[index()] += 2;
             return values[0] == 42 && calls == 1;
           })",
        R"(let calls = 0;
           func index(): int { calls += 1; return 0; }
           func main(): bool {
             const values: dynamic = [40]; values[index()] += 2;
             return (values[0] as int) == 42 && calls == 1;
           })",
        R"(let receivers = 0; let reads = 0; let writes = 0;
           class Box {
             value: int = 40;
             get item(): int { reads += 1; return this.value; }
             set item(next: int): void { writes += 1; this.value = next; }
           }
           func receiver(box: Box): Box { receivers += 1; return box; }
           func main(): bool {
             const box = Box(); receiver(box).item += 2;
             return box.value == 42 && receivers == 1 && reads == 1 && writes == 1;
           })",
        R"(class State { value: int; bump(): bool { this.value = this.value + 1; return false; } }
           func main(): bool { const state = State(value: 0); while (state.bump() || true) {
             break;
           } return state.value == 1; })"));

TEST(ControlFlowCompilerTest, OmitsUnreachableLoopIncrementAndBackEdge) {
  const auto compiled = ir::test::Prepare(
      "func main(): int { for (let i = 0; i < 2; i = i + 1) { return 1; } return 2; }");
  ASSERT_TRUE(compiled.Succeeded());
  const auto entry = std::ranges::find(compiled.program.functions, "main", &ir::Function::name);
  ASSERT_NE(entry, compiled.program.functions.end());
  EXPECT_FALSE(
      std::ranges::any_of(entry->instructions, [](const ir::Instruction& instruction) -> bool {
        return instruction.opcode == Opcode::AddInt || instruction.opcode == Opcode::Jump;
      }));
}

TEST(ControlFlowCompilerTest, CompilesAnInfiniteLoopWithoutRegisterOperands) {
  const auto compiled = ir::test::Prepare("func spin(): int { for (;;) {} }");
  ASSERT_TRUE(compiled.Succeeded());
  const auto function = std::ranges::find(compiled.program.functions, "spin", &ir::Function::name);
  ASSERT_NE(function, compiled.program.functions.end());
  ASSERT_EQ(function->instructions.size(), 1U);
  EXPECT_EQ(function->instructions.front().opcode, Opcode::Jump);
  EXPECT_EQ(function->instructions.front().index, 0U);
}

}  // namespace
}  // namespace vela::compiler
