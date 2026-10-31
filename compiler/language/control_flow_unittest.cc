#include <algorithm>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/bytecode/compiler.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate.h"
#include "vm/bytecode.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class FlowExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(FlowExecutionTest, ExecutesEveryReachableExit) {
  runtime::Isolate isolate;
  const auto entry = test::CompileEntry(GetParam(), isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = vm::VirtualMachine(isolate).Execute(entry->program(), entry->index());
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
        R"(class State { value: int; bump(): bool { this.value = this.value + 1; return false; } }
           func main(): bool { const state = State(value: 0); while (state.bump() || true) {
             break;
           } return state.value == 1; })"));

TEST(ControlFlowCompilerTest, OmitsUnreachableLoopIncrementAndBackEdge) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(
      "func main(): int { for (let i = 0; i < 2; i = i + 1) { return 1; } return 2; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto entry = compiled.program.FindFunction("main");
  ASSERT_TRUE(entry.IsValid());
  EXPECT_FALSE(std::ranges::any_of(entry.code(), [](vm::Instruction instruction) -> bool {
    const auto opcode = vm::DecodeOp(instruction);
    return opcode == vm::OpCode::AddInt || opcode == vm::OpCode::Jump;
  }));
}

TEST(ControlFlowCompilerTest, CompilesAnInfiniteLoopWithoutRegisterOperands) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("func spin(): int { for (;;) {} }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto function = compiled.program.FindFunction("spin");
  ASSERT_TRUE(function.IsValid());
  ASSERT_EQ(function.code().size(), 1U);
  EXPECT_EQ(function.register_count(), 1U);
  EXPECT_EQ(vm::DecodeOp(function.code().front()), vm::OpCode::Jump);
  EXPECT_EQ(vm::DecodeImm16(function.code().front()), -1);
}

}  // namespace
}  // namespace vela::compiler
