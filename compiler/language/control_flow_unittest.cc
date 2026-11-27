#include <algorithm>
#include <string>
#include <string_view>

#include "base/opcode.h"
#include "gtest/gtest.h"

#include "compiler/ir/ir.h"
#include "compiler/test_support/execution_expectations.h"
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

TEST(ControlFlowCompilerTest, NeverCallPreservesUnwindingAndNarrowsTheContinuingPath) {
  test::ExpectRunAnswer(R"(
class Failure {}
let cleaned = 0;
func fail(): never { throw Failure(); }
func require(value: int?): int { if (value == null) fail(); return value; }
func main(): int {
  try { try { return require(null); } finally { cleaned = 1; } }
  catch (error: Failure) { return require(41) + cleaned; }
})");
}

TEST(ControlFlowCompilerTest, NeverFunctionCannotFinishNormally) {
  const auto compiled = ir::test::Prepare("func invalid(): never { return; }");
  EXPECT_FALSE(compiled.Succeeded());
}

TEST(ControlFlowCompilerTest, ConditionAliasesRetainFactsAcrossBooleanComposition) {
  test::ExpectRunAnswer(R"(
func require(value: int?): int {
  const present = value != null;
  const valid = present && value > 0;
  if (!valid) return 0;
  return value;
}
func main(): int { return require(42); }
)");
}

TEST(ControlFlowCompilerTest, ConditionAliasesAreInvalidatedByWritesAndBranchMerges) {
  const auto compiled = ir::test::Prepare(R"(
func read(value: int?, reset: bool): int {
  const present = value != null;
  if (reset) value = null;
  if (present) return value;
  return 0;
}
)");
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
    return diagnostic.message == "cannot return 'int?' from function returning 'int'";
  }));
}

TEST(ControlFlowCompilerTest, ConditionAliasInvalidationDoesNotDiscardTheOtherBranch) {
  test::ExpectRunAnswer(R"(
func read(value: int?, reset: bool): int {
  const present = value != null;
  if (reset) { value = null; return 0; }
  if (present) return value;
  return 0;
}
func main(): int { return read(42, false); }
)");
}

TEST(ControlFlowCompilerTest, CapturedWritesInvalidateConditionAliasesAfterCalls) {
  const auto compiled = ir::test::Prepare(R"(
func read(): int {
  let value: int? = 42;
  const present = value != null;
  const reset = (): void => { value = null; };
  reset();
  if (present) return value;
  return 0;
}
)");
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
    return diagnostic.message == "cannot return 'int?' from function returning 'int'";
  }));
}

TEST(ControlFlowCompilerTest, PredicateFalseBranchesNarrowNamedAndCapturedFunctionValues) {
  test::ExpectRunAnswer(R"(
func isMissing(value: int?): bool { return value == null; }
func require(value: int?): int {
  const predicate = isMissing;
  const missing = predicate(value);
  if (missing) return 0;
  return value;
}
func main(): int { return require(42); }
)");
}

TEST(ControlFlowCompilerTest, PredicateFalseBranchRequiresItsOwnProof) {
  const auto compiled = ir::test::Prepare(R"(
func isPositive(value: int?): bool { return value != null && value > 0; }
func invalid(value: int?): null {
  if (isPositive(value)) return null;
  return value;
}
)");
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
    return diagnostic.message == "cannot return 'int?' from function returning 'null'";
  }));
}

TEST(ControlFlowCompilerTest, NegativeTypeTestsNarrowOnlyRepresentableComplements) {
  test::ExpectRunAnswer(R"(
func read(value: int?): int {
  if (!(value is int)) return 0;
  return value;
}
func main(): int { return read(42); }
)");
}

TEST(ControlFlowCompilerTest, KnownNullBranchKeepsNullableReceiverDeclarationForSafeAccess) {
  test::ExpectRunAnswer(R"(
func size(value: string?): int {
  if (value == null) return value?.length ?? 42;
  return value.length;
}
func main(): int { return size(null); }
)");
}

TEST(ControlFlowCompilerTest, ShortCircuitNeverBranchPreservesContinuingRefinements) {
  test::ExpectRunAnswer(R"(
func fail(): never { throw Error(message: 'missing'); }
func read(value: int?): int {
  value != null || fail();
  return value;
}
func main(): int { return read(42); }
)");
}

TEST(ControlFlowCompilerTest, SuspensionInvalidatesCapturedAliasDependencies) {
  const auto compiled = ir::test::Prepare(R"(
async func read(): Future<int> {
  let value: int? = 42;
  const reset = (): void => { value = null; };
  const pending = Future.run(reset);
  const present = value != null;
  await pending;
  if (present) return value;
  return 0;
}
)");
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
    return diagnostic.message == "cannot return 'int?' from function returning 'int'";
  }));
}

TEST(ControlFlowCompilerTest, LoopWritesInvalidateConditionAliases) {
  const auto compiled = ir::test::Prepare(R"(
func read(value: int?, again: bool): int {
  const present = value != null;
  while (again) { value = null; break; }
  if (present) return value;
  return 0;
}
)");
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
    return diagnostic.message == "cannot return 'int?' from function returning 'int'";
  }));
}

TEST(ControlFlowCompilerTest, ConditionAliasesKeepBindingIdentityAcrossShadowing) {
  test::ExpectRunAnswer(R"(
func read(value: int?): int {
  const present = value != null;
  { let value: int? = null; value = 1; }
  if (present) return value;
  return 0;
}
func main(): int { return read(42); }
)");
}

TEST(ControlFlowCompilerTest, AssignmentNarrowsNominalTypesUntilTheirNextWrite) {
  test::ExpectRunAnswer(R"(
class Base {}
class Child extends Base { answer: int = 42; }
func main(): int { let value: Base = Child(); return value.answer; }
)");
  const auto compiled = ir::test::Prepare(R"(
class Base {}
class Child extends Base { answer: int = 42; }
func read(reset: bool): int {
  let value: Base = Child();
  if (reset) value = Base();
  return value.answer;
}
)");
  EXPECT_FALSE(compiled.Succeeded());
}

TEST(ControlFlowCompilerTest, NeverCollectionElementsDoNotConstrainLaterElementTypes) {
  test::ExpectRunAnswer(R"(
func fail(): never { throw Error(message: 'missing'); }
func main(): int {
  try { const values = [fail(), 1]; }
  catch (error: Error) {
    try { const values = {fail(): 1, 'answer': 42}; }
    catch (error: Error) { return 42; }
  }
}
)");
}

TEST(ControlFlowCompilerTest, NeverCallbacksPreserveEmptyAndThrowingArrayPaths) {
  test::ExpectRunAnswer(R"(
func fail(): never { throw Error(message: 'missing'); }
func main(): int {
  const empty: int[] = [];
  empty.sort((_left, _right) => fail());
  const mapped = empty.map(_value => fail());
  if (mapped.length != 0) return 0;
  try { [2, 1].sort((_left, _right) => fail()); }
  catch (error: Error) { return 42; }
  return 0;
}
)");
}

TEST(ControlFlowCompilerTest, NullTypeMatchesNullableNever) {
  test::ExpectRunAnswer(R"(
func identity(value: null): never? { return value; }
func main(): int { const value: null = identity(null); return value == null ? 42 : 0; }
)");
}

TEST(ControlFlowCompilerTest, NullTypeCannotBeMadeNullableAgain) {
  const auto compiled = ir::test::Prepare("func invalid(value: null?): void {}");
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
    return diagnostic.message == "type is already nullable";
  }));
}

TEST(ControlFlowCompilerTest, NullTypeWorksAsAGenericArgumentAndBound) {
  test::ExpectRunAnswer(R"(
func identity<T>(value: T): T { return value; }
func onlyNull<T extends null>(value: T): T { return value; }
func main(): int {
  const first: null = identity(null);
  const second: null = onlyNull<null>(first);
  return second == null ? 42 : 0;
}
)");
}

TEST(ControlFlowCompilerTest, NullTypeIsInferredForValuesAndLambdaResults) {
  test::ExpectRunAnswer(R"(
const global = null;
func main(): int {
  const local = null;
  const produce = () => null;
  const result: null = produce();
  return global == local && result == null ? 42 : 0;
}
)");
}

TEST(ControlFlowCompilerTest, NullTypeIsInferredInCollections) {
  test::ExpectRunAnswer(R"(
func main(): int {
  const values: null[] = [null];
  const inferred = [null];
  const mapped = [1].map(value => value == 1 ? null : null);
  const entries = {'key': null};
  return values[0] == null && inferred[0] == null && mapped[0] == null &&
         entries.get('key') == null ? 42 : 0;
}
)");
}

TEST(ControlFlowCompilerTest, NullableGenericNeverIsNull) {
  test::ExpectRunAnswer(R"(
func empty<T>(): T? { return null; }
func main(): int {
  const value: null = empty<never>();
  return value == null ? 42 : 0;
}
)");
}

TEST(ControlFlowCompilerTest, NeverForInitializerOrConditionCannotReachFollowingStatement) {
  for (const auto* const source : {
           "class Failure {} func fail(): never { throw Failure(); } "
           "func f(): int { for (let x = fail(); true;) {} return 1; }",
           "class Failure {} func fail(): never { throw Failure(); } "
           "func f(): int { for (; fail();) {} return 1; }",
       }) {
    const auto compiled = ir::test::Prepare(source);
    EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
      return diagnostic.message == "unreachable statement";
    }));
  }
}

TEST(ControlFlowCompilerTest, NestedNeverArgumentStopsItsEnclosingCall) {
  const auto compiled = ir::test::Prepare(
      "class Failure {} func fail(): never { throw Failure(); } "
      "func consume(value: int): void {} "
      "func f(): int { consume(fail()); return 1; }");
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
    return diagnostic.message == "unreachable statement";
  }));
}

TEST(ControlFlowCompilerTest, ShortCircuitCanSkipNeverCall) {
  const auto compiled = ir::test::Prepare(
      "class Failure {} func fail(): never { throw Failure(); } "
      "func f(flag: bool): bool { flag && fail(); return true; }");
  EXPECT_TRUE(compiled.Succeeded());
}

TEST(ControlFlowCompilerTest, NestedNeverBranchPreservesOtherResult) {
  test::ExpectRunAnswer(R"(
class Failure {}
func fail(): never { throw Failure(); }
func consume(value: int): int { return value; }
func choose(flag: bool): int { return flag ? consume(fail()) : 42; }
func main(): int { return choose(false); }
)");
}

TEST(ControlFlowCompilerTest, NestedNeverArgumentPropagatesException) {
  test::ExpectRunAnswer(R"(
class Failure {}
func fail(): never { throw Failure(); }
func consume(value: int): int { return value; }
func f(): int { consume(fail()); }
func main(): int { try { return f(); } catch (error: Failure) { return 42; } }
)");
}

TEST(ControlFlowCompilerTest, ConstructorMergesOnlyNormallyCompletingExpressionBranches) {
  test::ExpectRunAnswer(R"(
class Failure {}
func fail(): never { throw Failure(); }
class Box {
  value: int;
  constructor(flag: bool) { flag ? fail() : (this.value = 42); }
}
func main(): int { return Box(false).value; }
)");
}

TEST(ControlFlowCompilerTest, ConstructorCatchMustInitializeAfterNeverCall) {
  const auto compiled = ir::test::Prepare(R"(
class Failure {}
func fail(): never { throw Failure(); }
class Box {
  value: int;
  constructor() { try { this.value = fail(); } catch (error: Failure) {} }
}
)");
  EXPECT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
    return diagnostic.message.find("must be initialized") != std::string::npos;
  }));
}

TEST(ControlFlowCompilerTest, NeverLoopAndSwitchSelectorsPropagateExceptions) {
  test::ExpectRunAnswer(R"(
class Failure {}
func fail(): never { throw Failure(); }
func first(): int { for (const _value in fail()) { break; } }
func second(): int { switch (fail()) { case 1: break; } }
func third(): int { while (fail()) { break; } }
func main(): int {
  let result = 0;
  try { first(); } catch (error: Failure) { result += 14; }
  try { second(); } catch (error: Failure) { result += 14; }
  try { third(); } catch (error: Failure) { result += 14; }
  return result;
}
)");
}

}  // namespace
}  // namespace vela::compiler
