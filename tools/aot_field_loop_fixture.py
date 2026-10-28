"""Field loop folding preserves aliases and observable overflow prefixes."""

SOURCE = """
class Box { public param value: int; }
func increase(box: Box, bound: int): void {
  for (let index = 0; index < bound; index++) { box.value += 1; }
}
func decrease(box: Box, bound: int): void {
  for (let index = 0; index < bound; index++) { box.value -= 1; }
}
export func calculate(operation: int, initial: int, bound: int): int {
  const box = Box(value: initial);
  const alias = box;
  if (operation == 0) { increase(box, bound); return alias.value; }
  if (operation == 1) { decrease(box, bound); return alias.value; }
  if (operation == 2) {
    try { increase(box, bound); }
    catch (error: Error) { if (error.code != 'IntegerOverflow') { return 0; } }
    finally { alias.value -= 1; }
    return box.value;
  }
  if (operation == 3) {
    try { decrease(box, bound); }
    catch (error: Error) { if (error.code != 'IntegerOverflow') { return 0; } }
    finally { alias.value += 1; }
    return box.value;
  }
  if (operation == 4) {
    let index = 0;
    try { for (; index < bound; index++) { box.value += 1; } }
    catch (error: Error) { return index; }
    return index;
  }
  if (operation == 5) {
    let effects = 0;
    try { for (let index = 0; index < bound; index++) { effects += 1; box.value += 1; } }
    catch (error: Error) { return effects; }
    return effects;
  }
  if (operation == 6) {
    for (let index = 0; index < bound; index++) { box.value += 2; }
    return box.value;
  }
  return 0;
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* FIELD_LOOP_IMAGE() noexcept;

TEST(NativeFieldLoopTest, ExactResultsAndFailurePrefixesSurviveAliasingAndFinally) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *FIELD_LOOP_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const std::array<std::array<std::int64_t, 4>, 13> cases{{
      {0, 7, -1, 7}, {0, maximum, 0, maximum}, {0, 0, 1000, 1000},
      {0, minimum, maximum, -1}, {1, maximum, maximum, 0},
      {1, 42, -1, 42}, {2, maximum - 3, 10, maximum - 1},
      {2, maximum, 1, maximum - 1}, {3, minimum + 3, 10, minimum + 1},
      {3, minimum, 1, minimum + 1}, {4, maximum - 3, 10, 3},
      {5, maximum - 3, 10, 4}, {6, 0, 1000, 2000},
  }};
  for (const auto& item : cases) {
    SCOPED_TRACE(::testing::PrintToString(item));
    const std::array arguments{Integer(item[0]), Integer(item[1]), Integer(item[2])};
    NativeValue output;
    ASSERT_EQ(Execute(FIELD_LOOP_ENTRY, execution, arguments, output), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), item[3]);
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}
'''
