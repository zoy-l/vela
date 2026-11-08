"""AOT recursion preserves roots, bounded execution, effects and complete failure traces."""

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
class Branch { value: int; next: Branch? = null; }
func walk(node: Branch?, depth: int, fail: bool): int {
  if (node == null) { return 0; }
  if (depth == 0) { return fail ? node.value + 9223372036854775807 : node.value; }
  const result = walk(node.next, depth - 1, fail);
  node.value += result;
  return node.value;
}
export func calculate(operation: int): int {
  if (operation == 0) { return fibonacci(31) == 1346269 ? 42 : 0; }
  if (operation == 1) { descend(41); return 42; }
  if (operation == 2) { return overflow(20, 9223372036854775807); }
  if (operation == 3) { effects = 0; return observed(41); }
  if (operation == 4) { return endless(); }
  if (operation == 5 || operation == 6 || operation == 7) {
    let root = Branch(value: 1);
    for (let index = 0; index < 24; index++) { root = Branch(value: 1, next: root); }
    if (operation == 5) { return walk(root, 24, false) == 25 ? 42 : 0; }
    if (operation == 6) { return walk(root, 24, true); }
    if (operation == 7) {
      try { walk(root, 24, true); }
      catch (error: Error) {
        let count = 0;
        let current: Branch? = root;
        while (current != null) { count += current.value; current = current.next; }
        return count == 25 && error.code == 'IntegerOverflow' ? 42 : 0;
      }
      return 0;
    }
  }
  return 0;
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
  ASSERT_NO_FATAL_FAILURE(ExpectOverflowTrace(RECURSIVE_ENTRY, execution, 2, 22, "overflow"));
  const std::array endless{Integer(4)};
  EXPECT_EQ(Execute(RECURSIVE_ENTRY, execution, endless, output), ExecutionStatus::StackOverflow);
  ASSERT_TRUE(execution.HasException());
  EXPECT_EQ(execution.ExceptionDiagnostic().stack_trace.size(), vela::kMaxCallFrames);
  EXPECT_EQ(execution.current_frame(), nullptr);
  EXPECT_EQ(*isolate.heap().native_stack_roots_head(), nullptr);
  ExpectIntegerOperations(RECURSIVE_ENTRY, execution, {1});
}

TEST_F(NativeRecursiveTest, ManagedRecursiveFailuresKeepOuterCatchRootsAndFullTraces) {
  NativeExecution execution(*instance);
  ExpectIntegerOperations(RECURSIVE_ENTRY, execution, {5, 7});
  ASSERT_NO_FATAL_FAILURE(ExpectOverflowTrace(RECURSIVE_ENTRY, execution, 6, 26, "walk"));
  // 溢出与 GC 之后再次调用，验证异常展开已释放资源且同一执行仍可继续。
  ExpectIntegerOperations(RECURSIVE_ENTRY, execution, {5, 7});
}
'''
