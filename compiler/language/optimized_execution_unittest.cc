#include <cstdint>
#include <string_view>

#include "base/execution_status.h"
#include "base/opcode.h"
#include "gtest/gtest.h"

#include "compiler/ir/preparation_test_support.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

TEST(OptimizationTest, FoldsConstantsThroughBindingsAndRemovesTheirDeadProducers) {
  const auto compiled = ir::test::Prepare("func main(): int { let a=20; let b=22; return a+b; }");
  ASSERT_TRUE(compiled.Succeeded());
  const auto& code = compiled.program.functions.front().instructions;
  ASSERT_EQ(code.size(), 2U);
  EXPECT_EQ(code.front().opcode, Opcode::LoadInt);
  EXPECT_EQ(code.front().immediate, 42);
  EXPECT_EQ(code.back().opcode, Opcode::Return);
}

TEST(OptimizationTest, PropagatesCopiesWithoutReadingOverwrittenSourceBindings) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(R"(
func compute(value: int): int {
  let original = value; let copy = original; original = 0;
  let second = copy; copy = 0;
  return second + original + copy;
}
func main(): int { return compute(42); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = test::Execute(*compiled.instance, "main");
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

struct OptimizedProgram final {
  std::string_view source;
  std::int64_t expected;
};

class OptimizedExecutionTest : public testing::TestWithParam<OptimizedProgram> {};

TEST_P(OptimizedExecutionTest, PreservesControlFlowAndLiveValues) {
  const auto& parameter = GetParam();
  SCOPED_TRACE(parameter.source);
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(parameter.source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, parameter.expected);
}

INSTANTIATE_TEST_SUITE_P(
    BranchesLoopsCallsAndArithmetic,
    OptimizedExecutionTest,
    testing::Values(OptimizedProgram{R"(func choose(flag: bool): int {
          let value=1; if (flag) value=2; else value=3; return value+4;
        } func main(): int { return choose(true)+choose(false); })",
                                     13},
                    OptimizedProgram{R"(func main(): int {
          let result=0; for (let i=0; i<10; i=i+1) { result=result+i; } return result;
        })",
                                     45},
                    OptimizedProgram{R"(func main(): int {
          let total=0; let i=0; while(i<8) { i=i+1;
            switch(i) { case 2: continue; case 5: break; default: total=total+i; }
            total=total+10;
          } return total;
        })",
                                     99},
                    OptimizedProgram{R"(class Counter { value: int;
          next(): int { this.value=this.value+1; return this.value; }
        } func combine(first: int, second: int, third: int): int { return first*100+second*10+third; }
        func identity(value: int): int { return value; }
        func main(): int { const c=Counter(value: 0);
          return combine(third: c.next(), first: identity(c.next()), second: c.next());
        })",
                                     231},
                    OptimizedProgram{R"(func main(): int { return 30000*30000*30000*30000; })",
                                     810000000000000000LL},
                    OptimizedProgram{R"(func main(): int { return -32768; })", -32768},
                    OptimizedProgram{R"(func main(): int {
          let i=0; let value: int? = null;
          while(i<3) { value=i; i=i+1; }
          if(value!=null) return value+40; return 0;
        })",
                                     42}));

struct TrappingProgram final {
  std::string_view source;
  ExecutionStatus status;
};

class OptimizerTrapTest : public testing::TestWithParam<TrappingProgram> {};

TEST_P(OptimizerTrapTest, RetainsFailuresEvenWhenTheResultIsUnused) {
  const auto& parameter = GetParam();
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(parameter.source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.instance->FindFunction("main");
  ASSERT_TRUE(main.has_value());
  EXPECT_EQ(test::Execute(*compiled.instance).status, parameter.status);
}

INSTANTIATE_TEST_SUITE_P(
    ErrorsAreObservable,
    OptimizerTrapTest,
    testing::Values(
        TrappingProgram{"func main(): int { const unused=1/0; return 42; }",
                        ExecutionStatus::DivisionByZero},
        TrappingProgram{
            "func main(): int { const unused=30000*30000*30000*30000*30000; return 42; }",
            ExecutionStatus::IntegerOverflow},
        TrappingProgram{"func main(): int { const values=[1]; const unused=values[2]; return 42; }",
                        ExecutionStatus::IndexOutOfBounds}));

TEST(OptimizationTest, ReleasesFrameRootsAfterErrorsAndBeforeNextInvocation) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileSource(R"(
class Box { value: int; }
func fail(): int { const box=Box(value: 1); return box.value/0; }
func main(): int { return fail(); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.instance->FindFunction("main");
  ASSERT_TRUE(main.has_value());
  const auto baseline = isolate.heap().object_count();
  for (int attempt = 0; attempt < 2; ++attempt) {
    EXPECT_EQ(test::Execute(*compiled.instance).status, ExecutionStatus::DivisionByZero);
    isolate.heap().FinishCollection();
    isolate.heap().Collect();
    EXPECT_EQ(isolate.heap().object_count(), baseline);
  }
}

}  // namespace
}  // namespace vela::compiler
