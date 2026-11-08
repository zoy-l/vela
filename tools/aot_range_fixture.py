"""AOT range specialization preserves generic entries and checked failure order."""

SOURCE = r'''
func fits(values: int[], row: int, column: int): bool {
  for (let index = 0; index < row; index++) {
    const placed = values[index];
    if (placed == column || placed - index == column - row || placed + index == column + row) {
      return false;
    }
  }
  return true;
}
func search(values: int[], row: int): int {
  if (row == 5) { return 1; }
  let count = 0;
  for (let column = 0; column < 5; column++) {
    if (fits(values, row, column)) { values[row] = column; count += search(values, row + 1); }
  }
  return count;
}
func even(values: int[], step: int): int {
  if (step == 6) { return values[5] + 37; }
  values[step] = step;
  return odd(values, step + 1);
}
func odd(values: int[], step: int): int { return even(values, step); }
func prefix(values: int[], limit: int, column: int): bool {
  for (let index = 0; index < limit; index++) { if (values[index] == column) { return false; } }
  return true;
}
func readSlot(values: int[], index: int): int { return values[index] + 1; }
func readFirst(values: int[]): int { return values[0]; }
func increment(value: int): int { return value + 1; }
func makeWide(): int[] { return [9223372036854775807]; }
func makeEmpty(): int[] { return []; }
func storeThenFail(values: int[], index: int): int { values[0] = 41; return values[index] + 1; }
export func calculate(operation: int): int {
  if (operation == 0) { return search([0, 0, 0, 0, 0], 0) == 10 ? 42 : 0; }
  if (operation == 1) { return readSlot([41], 0); }
  if (operation == 2) { return !prefix([0], 2, 0) ? 42 : 0; }
  if (operation == 3) {
    try { prefix([0], 2, 7); }
    catch (error: Error) { return error.code == 'IndexOutOfBounds' ? 42 : 0; }
    return 0;
  }
  if (operation == 4) { return makeWide().length == 1 ? 42 : 0; }
  if (operation == 5) { return even([0, 0, 0, 0, 0, 0], 0); }
  if (operation == 6) { return increment(41); }
  if (operation == 8) {
    return readFirst([-9223372036854775808, 9223372036854775807]) == -9223372036854775808 ? 42 : 0;
  }
  if (operation == 9) { return makeEmpty().length == 0 ? 42 : 0; }
  const values = [0];
  try { storeThenFail(values, 2); }
  catch (error: Error) { return values[0] == 41 && error.code == 'IndexOutOfBounds' ? 42 : 0; }
  return 0;
}
'''

HOST_TESTS = r'''
extern "C" const NativeImage* RANGE_IMAGE() noexcept;
using NativeRangeTest = NativeImageTest<RANGE_IMAGE, 1>;

TEST_F(NativeRangeTest, BoundedArraysAndMutualRecursionKeepResultsUnderGcPressure) {
  NativeExecution execution(*instance);
  ExpectIntegerOperations(RANGE_ENTRY, execution, {0, 1, 4, 5, 6, 8, 9});
  EXPECT_EQ(*isolate.heap().native_stack_roots_head(), nullptr);
}

TEST_F(NativeRangeTest, ShortArraysKeepEarlyReturnsAndObservableFailurePrefixes) {
  NativeExecution execution(*instance);
  ExpectIntegerOperations(RANGE_ENTRY, execution, {2, 3, 7});
  EXPECT_EQ(*isolate.heap().native_stack_roots_head(), nullptr);
}

TEST_F(NativeRangeTest, TrustedInternalEntriesAcceptValuesOutsideSpecializedRanges) {
  NativeExecution allocation(*instance);
  auto values = InvokeInternal(allocation, "makeWide");
  ASSERT_TRUE(values);
  RootRange root(isolate.heap(), &*values, 1);
  isolate.heap().Collect();
  for (const auto index : {std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}}) {
    NativeExecution execution(*instance);
    const std::array arguments{*values, Value::Integer(index)};
    ExpectInternalFailure(execution, "readSlot", arguments,
                          index == 0 ? ExecutionStatus::IntegerOverflow
                                     : ExecutionStatus::IndexOutOfBounds);
  }
  NativeExecution execution(*instance);
  const std::array arguments{Value::Integer(std::numeric_limits<std::int64_t>::max())};
  ExpectInternalFailure(execution, "increment", arguments, ExecutionStatus::IntegerOverflow);
}

TEST_F(NativeRangeTest, FixedLengthSpecializationKeepsTheGenericEmptyArrayCheck) {
  NativeExecution execution(*instance);
  auto values = InvokeInternal(execution, "makeEmpty");
  ASSERT_TRUE(values);
  RootRange root(isolate.heap(), &*values, 1);
  isolate.heap().Collect();
  const std::array arguments{*values};
  ExpectInternalFailure(execution, "readFirst", arguments, ExecutionStatus::IndexOutOfBounds);
}
'''
