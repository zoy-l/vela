"""Pure AOT recursion preserves bounded execution, effects and complete failure traces."""

SOURCE = r'''
let effects = 0;
func fibonacci(value: int): int {
  if (value < 2) { return value; }
  return fibonacci(value - 1) + fibonacci(value - 2);
}
func descend(depth: int): void {
  if (depth > 0) { descend(depth - 1); }
}
func observed(depth: int): int {
  effects += 1;
  if (depth == 0) { return effects; }
  return observed(depth - 1);
}
func overflow(depth: int, value: int): int {
  if (depth == 0) { return value + 1; }
  return overflow(depth - 1, value);
}
func endless(): int { return endless(); }
export func calculate(operation: int): int {
  if (operation == 0) { return fibonacci(31) == 1346269 ? 42 : 0; }
  if (operation == 1) { descend(41); return 42; }
  if (operation == 2) { return overflow(20, 9223372036854775807); }
  if (operation == 3) { effects = 0; return observed(41); }
  return endless();
}
'''

HOST_TESTS = r'''
extern "C" const NativeImage* RECURSIVE_IMAGE() noexcept;
using NativeRecursiveTest = NativeImageTest<RECURSIVE_IMAGE, 1>;

TEST_F(NativeRecursiveTest, BoundedRecursionHandlesDeeperCallsAndDoesNotReplayEffects) {
  NativeExecution execution(*instance);
  ExpectIntegerOperations(RECURSIVE_ENTRY, execution, {0, 1, 3});
}

TEST_F(NativeRecursiveTest, OverflowAndExhaustedStacksKeepCompleteTracesAndUnwind) {
  NativeExecution execution(*instance);
  NativeValue output;
  const std::array overflow{Integer(2)};
  ASSERT_EQ(Execute(RECURSIVE_ENTRY, execution, overflow, output), ExecutionStatus::IntegerOverflow);
  ASSERT_TRUE(execution.HasException());
  isolate.heap().Collect();
  const auto& trace = execution.ExceptionDiagnostic().stack_trace;
  ASSERT_EQ(trace.size(), 22U);
  EXPECT_EQ(trace.front().function, "overflow");
  EXPECT_EQ(trace.back().function, "calculate");
  EXPECT_EQ(execution.current_frame(), nullptr);
  const std::array endless{Integer(4)};
  EXPECT_EQ(Execute(RECURSIVE_ENTRY, execution, endless, output), ExecutionStatus::StackOverflow);
  ASSERT_TRUE(execution.HasException());
  EXPECT_EQ(execution.ExceptionDiagnostic().stack_trace.size(), vela::kMaxCallFrames);
  EXPECT_EQ(execution.current_frame(), nullptr);
  EXPECT_EQ(*isolate.heap().native_stack_roots_head(), nullptr);
  ExpectIntegerOperations(RECURSIVE_ENTRY, execution, {1});
}
'''
