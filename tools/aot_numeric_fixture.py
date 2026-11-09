"""AOT numeric conversion boundaries and failure roots in standalone execution."""

SOURCE = """
func truncate(value: double): int { return value.toInt(); }
class SortItem { key: int; order: int; text: string; }
export func calculate(operation: int, integer: int, decimal: double): dynamic {
  if (operation == 0) { return integer.toDouble(); }
  if (operation == 1) { return truncate(decimal); }
  if (operation == 2) { return integer.toInt(); }
  if (operation == 3) { return decimal.toDouble(); }
  if (operation == 4) {
    const retained = ['retained:' + integer.toString()];
    let result = 0;
    try { result = truncate(decimal); }
    catch (error: Error) {
      if (error.code == 'InvalidNumericConversion' && retained[0] == 'retained:42') { result = 40; }
    } finally { result += 2; }
    return result;
  }
  if (operation == 6) {
    let value: int? = integer;
    if (integer == 0) { value = null; }
    return value != null ? value + 2 : 2;
  }
  if (operation == 7) {
    let value: int? = null;
    const selected = integer > 0 ? (value = integer) : (value = 2);
    return value;
  }
  if (operation == 8) {
    let value: string? = 'retained';
    if (integer <= 0) { value = null; }
    return value != null ? value.length : 0;
  }
  if (operation == 9) {
    const values = [0x2a, 0B101010, 0O52,
                    0x7fffffffffffffff, -0x8000000000000000,
                    0o777777777777777777777, -0o1000000000000000000000,
                    0b111111111111111111111111111111111111111111111111111111111111111,
                    -0b1000000000000000000000000000000000000000000000000000000000000000];
    return values[integer];
  }
  if (operation == 10) {
    const values = [0.0, 0.0, 0.0, 0.0];
    let completed = 0;
    try {
      for (let index = 0; index < integer; index++) {
        values[index] = Math.sqrt(index == 2 ? decimal : 4.0);
        completed++;
      }
    } catch (error: Error) {
      if (error.code != 'NativeFailure' || completed != 2 ||
          values[0] != 2.0 || values[1] != 2.0 || values[2] != 0.0) { return -1; }
      return 42;
    }
    return values[0] + values[1] + values[2] + values[3];
  }
  if (operation == 11) {
    const values: SortItem[] = [];
    for (let index = 0; index < integer; index++) {
      values.push(SortItem(key: (integer - index) & 3, order: index, text: index.toString()));
    }
    values.sort((left, right) => {
      const allocation = '${left.order}:${right.order}';
      return allocation.length > 0 ? left.key - right.key : 0;
    });
    for (let index = 0; index < integer; index++) {
      const current = values[index];
      if (current.text != current.order.toString()) { return 0; }
      if (index > 0) {
        const previous = values[index - 1];
        if (previous.key > current.key ||
            (previous.key == current.key && previous.order > current.order)) { return 0; }
      }
    }
    return 42;
  }
  if (operation == 13) {
    let sum = 0.0;
    for (let index = 0; index < integer; index++) {
      sum += Math.sqrt(index == 2 ? decimal : 4.0);
    }
    return sum;
  }
  if (operation == 12) {
    const values: int[] = [];
    for (let index = 0; index < 65; index++) { values.push(65 - index); }
    let comparisons = 0;
    try {
      values.sort((left, right) => {
        comparisons++;
        if (comparisons == integer) { throw Error(message: 'stop'); }
        return left - right;
      });
      return 0;
    } catch (error: Error) {
      if (error.message != 'stop') { return 0; }
    }
    for (let index = 0; index < 65; index++) {
      if (values[index] != 65 - index) { return 0; }
    }
    return 42;
  }
  let sum = 0.0;
  for (let index = 0; index < integer; index++) { sum += index.toDouble(); }
  return sum.toInt();
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* NUMERIC_IMAGE() noexcept;

double OverrideSquareRoot(double value) noexcept { return value + 1.0; }

TEST(NativeNumericTest, InvariantSqrtPreservesOverridesEmptyLoopsAndFailurePrefixes) {
  for (const auto overridden : {false, true}) {
    Isolate isolate(1);
    if (overridden) {
      ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<OverrideSquareRoot>("Math.sqrt")));
    }
    const auto instance = NativeInstance::Instantiate(isolate, *NUMERIC_IMAGE());
    ASSERT_TRUE(instance.has_value()) << instance.error().message;
    NativeExecution execution(**instance);
    for (const auto count : {0, 1, 4}) {
      const std::array arguments{Integer(10), Integer(count), Double(9.0)};
      NativeValue result;
      ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
      EXPECT_EQ(result.kind, NativeValueKind::Double);
      const auto expected = count == 0 ? 0.0 :
          (count == 1 ? (overridden ? 5.0 : 2.0) : (overridden ? 25.0 : 9.0));
      EXPECT_EQ(std::bit_cast<double>(result.payload), expected);
      EXPECT_EQ(execution.current_frame(), nullptr);
      EXPECT_FALSE(execution.HasException());
    }
    const std::array arguments{Integer(10), Integer(4), Double(-1.0)};
    NativeValue result;
    ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    if (overridden) {
      EXPECT_EQ(result.kind, NativeValueKind::Double);
      EXPECT_EQ(std::bit_cast<double>(result.payload), 15.0);
    } else {
      EXPECT_EQ(result.kind, NativeValueKind::Integer);
      EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42);
      const std::array unhandled{Integer(13), Integer(4), Double(-1.0)};
      ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, unhandled, result), ExecutionStatus::NativeFailure);
      isolate.heap().Collect();
      const auto& diagnostic = execution.ExceptionDiagnostic();
      EXPECT_EQ(diagnostic.message, "sqrt argument must be nonnegative");
      const auto& trace = diagnostic.stack_trace;
      ASSERT_FALSE(trace.empty());
      EXPECT_EQ(trace.back().function, "calculate");
      EXPECT_GT(trace.back().line, 0U);
      EXPECT_EQ(execution.current_frame(), nullptr);
      const std::array empty{Integer(13), Integer(0), Double(-1.0)};
      ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, empty, result), ExecutionStatus::Returned);
      EXPECT_EQ(std::bit_cast<double>(result.payload), 0.0);
      EXPECT_FALSE(execution.HasException());
    }
  }
}

TEST(NativeNumericTest, StableSortRetainsManagedKeysAcrossRunAndMergeBoundaries) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *NUMERIC_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  for (const auto count : {0, 1, 2, 31, 32, 33, 63, 64, 65, 127, 128, 129}) {
    SCOPED_TRACE(count);
    const std::array arguments{Integer(11), Integer(count), Double(0.0)};
    NativeValue result;
    ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(result.kind, NativeValueKind::Integer);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42);
  }
  for (const auto comparison : {1, 16, 32, 496, 993, 1025}) {
    SCOPED_TRACE(comparison);
    const std::array arguments{Integer(12), Integer(comparison), Double(0.0)};
    NativeValue result;
    ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(result.kind, NativeValueKind::Integer);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42);
  }
}

TEST(NativeNumericTest, RadixLiteralsPreserveValuesAndSignedEndpoints) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *NUMERIC_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
  constexpr auto kMinimum = std::numeric_limits<std::int64_t>::min();
  const std::array<std::int64_t, 9> expected{
      42, 42, 42, kMaximum, kMinimum, kMaximum, kMinimum, kMaximum, kMinimum};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    SCOPED_TRACE(index);
    const std::array arguments{Integer(9), Integer(static_cast<std::int64_t>(index)), Double(0.0)};
    NativeValue result;
    ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(result.kind, NativeValueKind::Integer);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), expected[index]);
  }
}

TEST(NativeNumericTest, IntegerConversionsRoundToBinary64AndPreserveIntegerIdentity) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *NUMERIC_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array<std::int64_t, 9> values{
      std::numeric_limits<std::int64_t>::min(), -9007199254740993LL, -1, 0, 1,
      9007199254740991LL, 9007199254740992LL, 9007199254740993LL,
      std::numeric_limits<std::int64_t>::max()};
  for (const auto value : values) {
    SCOPED_TRACE(value);
    for (const auto operation : {0, 2}) {
      const std::array arguments{Integer(operation), Integer(value), Double(0.0)};
      NativeValue result;
      ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
      if (operation == 0) {
        EXPECT_EQ(result.kind, NativeValueKind::Double);
        EXPECT_EQ(std::bit_cast<double>(result.payload), static_cast<double>(value));
      } else {
        EXPECT_EQ(result.kind, NativeValueKind::Integer);
        EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), value);
      }
      EXPECT_FALSE(execution.HasException());
      EXPECT_EQ(execution.current_frame(), nullptr);
    }
  }
}

TEST(NativeNumericTest, DoubleConversionsTruncateAndRejectNonFiniteOrOutOfRangeValues) {
  Isolate isolate(1);
  const auto* image = NUMERIC_IMAGE();
  const auto instance = NativeInstance::Instantiate(isolate, *image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array valid{-0x1p63, -9007199254740992.0, -42.75, -0.75, -0.0, 0.0,
                         0x1p-1074, 0.75, 42.75, 0x1.fffffffffffffp62};
  for (const auto value : valid) {
    SCOPED_TRACE(value);
    const std::array arguments{Integer(1), Integer(0), Double(value)};
    NativeValue result;
    ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(result.kind, NativeValueKind::Integer);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), static_cast<std::int64_t>(value));
  }
  const std::array invalid{-0x1.0000000000001p63, 0x1p63,
                           -std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()};
  for (const auto value : invalid) {
    const std::array arguments{Integer(1), Integer(0), Double(value)};
    NativeValue result;
    ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result),
              ExecutionStatus::InvalidNumericConversion);
    EXPECT_EQ(execution.current_frame(), nullptr);
    ASSERT_TRUE(execution.HasException());
    isolate.heap().Collect();
    const auto& diagnostic = execution.ExceptionDiagnostic();
    EXPECT_EQ(diagnostic.status, ExecutionStatus::InvalidNumericConversion);
    const auto& trace = diagnostic.stack_trace;
    ASSERT_FALSE(trace.empty());
    const auto& frame = trace.front();
    EXPECT_EQ(frame.function, "truncate");
    EXPECT_EQ(frame.line, 2U);
  }
}

TEST(NativeNumericTest, DoubleIdentityPreservesSignedZeroAndNanPayloads) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *NUMERIC_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array<std::uint64_t, 6> bits{0, 0x8000000000000000ULL, 1,
      0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000042ULL};
  for (const auto payload : bits) {
    const std::array arguments{Integer(3), Integer(0), Double(std::bit_cast<double>(payload))};
    NativeValue result;
    ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(result.kind, NativeValueKind::Double);
    EXPECT_EQ(result.payload, payload);
  }
}

TEST(NativeNumericTest, FailureRetainsCatchRootsAndLoopConversionsKeepZeroTripBehavior) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *NUMERIC_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array caught{Integer(4), Integer(42), Double(std::numeric_limits<double>::infinity())};
  NativeValue result;
  ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, caught, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42);
  const std::array<std::array<std::int64_t, 2>, 4> cases{{{-5, 0}, {0, 0}, {1, 0}, {500, 124750}}};
  for (const auto& item : cases) {
    const std::array arguments{Integer(5), Integer(item[0]), Double(0.0)};
    ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), item[1]);
    EXPECT_EQ(execution.current_frame(), nullptr);
    EXPECT_FALSE(execution.HasException());
  }
}

TEST(NativeNumericTest, ConditionalPromotionPreservesNullablePayloadsAndBothBranchWrites) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *NUMERIC_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array<std::array<std::int64_t, 3>, 6> cases{{
      {6, 40, 42}, {6, 0, 2}, {7, 40, 40}, {7, -1, 2}, {8, 40, 8}, {8, 0, 0}}};
  for (const auto& item : cases) {
    SCOPED_TRACE(item[0]);
    const std::array arguments{Integer(item[0]), Integer(item[1]), Double(0.0)};
    NativeValue result;
    ASSERT_EQ(Execute(NUMERIC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(result.kind, NativeValueKind::Integer);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), item[2]);
    EXPECT_EQ(execution.current_frame(), nullptr);
    EXPECT_FALSE(execution.HasException());
  }
}
'''
