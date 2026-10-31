#include <cstdint>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/program_test_support.h"
#include "runtime/isolate.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(OptimizationTest, FoldsConstantsThroughBindingsAndRemovesTheirDeadProducers) {
  runtime::Isolate isolate;
  const auto compiled =
      CompileSource("func main(): int { let a=20; let b=22; return a+b; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  const auto code = main.code();
  ASSERT_EQ(code.size(), 2U);
  EXPECT_EQ(vm::DecodeOp(code.front()), vm::OpCode::LoadInt);
  EXPECT_EQ(vm::DecodeImm16(code.front()), 42);
  EXPECT_EQ(vm::DecodeOp(code.back()), vm::OpCode::Return);
  EXPECT_EQ(main.register_count(), 1U);
}

TEST(OptimizationTest, PropagatesCopiesWithoutReadingOverwrittenSourceBindings) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
func compute(value: int): int {
  let original = value; let copy = original; original = 0;
  let second = copy; copy = 0;
  return second + original + copy;
}
func main(): int { return compute(42); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
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
  vm::ExecutionStatus status;
};

class OptimizerTrapTest : public testing::TestWithParam<TrappingProgram> {};

TEST_P(OptimizerTrapTest, RetainsFailuresEvenWhenTheResultIsUnused) {
  const auto& parameter = GetParam();
  runtime::Isolate isolate;
  const auto compiled = CompileSource(parameter.source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  EXPECT_EQ(vm::VirtualMachine(isolate).Execute(main).status, parameter.status);
}

INSTANTIATE_TEST_SUITE_P(
    ErrorsAreObservable,
    OptimizerTrapTest,
    testing::Values(
        TrappingProgram{"func main(): int { const unused=1/0; return 42; }",
                        vm::ExecutionStatus::DivisionByZero},
        TrappingProgram{
            "func main(): int { const unused=30000*30000*30000*30000*30000; return 42; }",
            vm::ExecutionStatus::IntegerOverflow},
        TrappingProgram{"func main(): int { const values=[1]; const unused=values[2]; return 42; }",
                        vm::ExecutionStatus::IndexOutOfBounds}));

TEST(OptimizationTest, ReleasesFrameRootsAfterErrorsAndBeforeRegisterReuse) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
class Box { value: int; }
func fail(): int { const box=Box(value: 1); return box.value/0; }
func main(): int { return fail(); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  vm::VirtualMachine machine(isolate);
  for (int attempt = 0; attempt < 2; ++attempt) {
    EXPECT_EQ(machine.Execute(main).status, vm::ExecutionStatus::DivisionByZero);
    isolate.heap().FinishCollection();
    isolate.heap().Collect();
    EXPECT_EQ(isolate.heap().object_count(), 0U);
  }
}

}  // namespace
}  // namespace vela::compiler
