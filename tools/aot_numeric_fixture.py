"""AOT numeric conversion boundaries and failure roots in standalone execution."""

SOURCE = """
func truncate(value: double): int { return value.toInt(); }
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
  let sum = 0.0;
  for (let index = 0; index < integer; index++) { sum += index.toDouble(); }
  return sum.toInt();
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* NUMERIC_IMAGE() noexcept;

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
      EXPECT_EQ(execution.frame_count(), 0U);
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
    EXPECT_EQ(execution.frame_count(), 0U);
    ASSERT_TRUE(execution.HasException());
    isolate.heap().Collect();
    const auto& error = ExceptionData::From(*execution.pending_exception().AsObject());
    EXPECT_EQ(error.status, ExecutionStatus::InvalidNumericConversion);
    const auto diagnostic = execution.ExceptionDiagnostic();
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
    EXPECT_EQ(execution.frame_count(), 0U);
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
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}
'''
