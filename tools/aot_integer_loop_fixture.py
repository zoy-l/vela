"""Checked integer loop reductions, including observable failure prefixes."""

SOURCE = """
let effects = 0;
func ascending(bound: int, initial: int): int {
  let sum = initial;
  for (let index = 0; index < bound; index++) { sum += index; }
  return sum;
}
func descending(bound: int, initial: int): int {
  let sum = initial;
  for (let index = 0; index > bound; index--) { sum += index; }
  return sum;
}
func constant(start: int, bound: int, initial: int, term: int): int {
  let sum = initial;
  for (let index = start; index < bound; index++) { sum += term; }
  return sum;
}
func subtract(bound: int, initial: int): int {
  let sum = initial;
  for (let index = 0; index < bound; index++) { sum -= index; }
  return sum;
}
func subtractConstant(bound: int, initial: int, term: int): int {
  let sum = initial;
  for (let index = 0; index < bound; index++) { sum -= term; }
  return sum;
}
func crossing(start: int, bound: int, initial: int): int {
  let sum = initial;
  for (let index = start; index < bound; index++) { sum += index; }
  return sum;
}
func observed(bound: int, initial: int): int {
  let sum = initial;
  try { for (let index = 0; index < bound; index++) { sum += index; } }
  catch (error: Error) { return sum; }
  return sum;
}
func sideEffects(bound: int, initial: int): int {
  effects = 0;
  let sum = initial;
  try {
    for (let index = 0; index < bound; index++) { effects += 1; sum += index; }
  } catch (error: Error) { return effects; }
  return sum;
}
func inclusive(start: int, bound: int): int {
  let sum = 0;
  for (let index = start; index <= bound; index++) { sum += 1; }
  return sum;
}
func caught(bound: int, initial: int): int {
  const retained = 'retained:' + initial.toString();
  let sum = initial;
  effects = 0;
  try {
    for (let index = 0; index < bound; index++) { sum += index; }
  } catch (error: Error) {
    return error.code == 'IntegerOverflow' && retained.length > 9 ? 42 : 0;
  } finally { effects = 7; }
  return sum;
}
export func calculate(operation: int, start: int, bound: int, initial: int, term: int): int {
  if (operation == 0) { return ascending(bound, initial); }
  if (operation == 1) { return descending(bound, initial); }
  if (operation == 2) { return constant(start, bound, initial, term); }
  if (operation == 3) { return subtract(bound, initial); }
  if (operation == 4) { return subtractConstant(bound, initial, term); }
  if (operation == 5) { return crossing(start, bound, initial); }
  if (operation == 6) { return observed(bound, initial); }
  if (operation == 7) { return sideEffects(bound, initial); }
  if (operation == 8) { return inclusive(start, bound); }
  if (operation == 9) { const result = caught(bound, initial); return result + effects; }
  return 0;
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* INTEGER_LOOP_IMAGE() noexcept;

TEST(NativeIntegerLoopTest, DynamicBoundsAndWideIntermediatesKeepExactResults) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *INTEGER_LOOP_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::array<std::array<std::int64_t, 6>, 20> cases{{
      {0, 0, -10, 7, 0, 7}, {0, 0, 0, maximum, 0, maximum}, {0, 0, 1, 7, 0, 7},
      {0, 0, 1000, 0, 0, 499500}, {0, 0, 3, -5, 0, -2},
      {0, 0, 4294967296LL, 0, 0, 9223372034707292160LL},
      {0, 0, 4294967297LL, minimum, 0, 2147483648LL},
      {1, 0, 10, 7, 0, 7}, {1, 0, -4, 0, 0, -6},
      {1, 0, -4294967297LL, maximum, 0, -2147483649LL},
      {2, -5, 5, 1, -2, -19}, {2, 9, 5, 42, maximum, 42},
      {2, minimum, maximum, 42, 0, 42},
      {2, minimum, maximum, minimum, 1, maximum},
      {2, minimum, maximum, maximum, -1, minimum},
      {2, 0, 1, maximum, minimum, -1},
      {3, 0, 4, 0, 0, -6}, {4, 0, 1, minimum, minimum, 0},
      {5, -4, 5, 17, 0, 17}, {9, 0, 4, maximum - 2, 0, 49},
  }};
  for (const auto& item : cases) {
    SCOPED_TRACE(::testing::PrintToString(item));
    const std::array arguments{Integer(item[0]), Integer(item[1]), Integer(item[2]),
                               Integer(item[3]), Integer(item[4])};
    NativeValue result;
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), item[5]);
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}

TEST(NativeIntegerLoopTest, OverflowKeepsItsStatusAndOriginalSourcePosition) {
  Isolate isolate(1);
  const auto* image = INTEGER_LOOP_IMAGE();
  const auto instance = NativeInstance::Instantiate(isolate, *image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::array<std::array<std::int64_t, 5>, 8> cases{{
      {0, 0, 4294967297LL, 0, 0}, {0, 0, maximum, 0, 0},
      {1, 0, -4294967297LL, 0, 0}, {1, 0, minimum, 0, 0},
      {2, minimum, maximum, 0, 1}, {3, 0, 3, minimum, 0},
      {4, 0, 1, 0, minimum}, {4, 0, 2, minimum, maximum},
  }};
  const std::array names{"ascending", "descending", "constant", "subtract", "subtractConstant"};
  const std::array<std::uint32_t, 5> lines{INTEGER_LOOP_TRACE_LINES};
  for (const auto& item : cases) {
    SCOPED_TRACE(::testing::PrintToString(item));
    const std::array arguments{Integer(item[0]), Integer(item[1]), Integer(item[2]),
                               Integer(item[3]), Integer(item[4])};
    NativeValue result;
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), ExecutionStatus::IntegerOverflow);
    EXPECT_EQ(execution.frame_count(), 0U);
    ASSERT_TRUE(execution.HasException());
    isolate.heap().Collect();
    const auto& exception = ExceptionData::From(*execution.pending_exception().AsObject());
    EXPECT_EQ(exception.status, ExecutionStatus::IntegerOverflow);
    EXPECT_EQ(exception.message(), "integer overflow");
    const auto trace = exception.trace();
    ASSERT_FALSE(trace.empty());
    const auto position = trace.front();
    const auto& function = image->functions.values()[position.function];
    const auto operation = static_cast<std::size_t>(item[0]);
    EXPECT_EQ(function.name.text(), names[operation]);
    EXPECT_EQ(function.locations.values()[position.instruction].line, lines[operation]);
  }
}

TEST(NativeIntegerLoopTest, ObservedPrefixesAndOtherFailureExitsRemainOrdered) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *INTEGER_LOOP_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::array<std::array<std::int64_t, 6>, 2> cases{{
      {6, 0, 4, maximum - 2, 0, maximum - 1}, {7, 0, 4, maximum - 2, 0, 3},
  }};
  for (const auto& item : cases) {
    const std::array arguments{Integer(item[0]), Integer(item[1]), Integer(item[2]),
                               Integer(item[3]), Integer(item[4])};
    NativeValue result;
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), item[5]);
  }
  const std::array crossing{Integer(5), Integer(-4), Integer(5), Integer(minimum + 2), Integer(0)};
  NativeValue result;
  EXPECT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, crossing, result), ExecutionStatus::IntegerOverflow);
  const std::array inclusive{Integer(8), Integer(maximum - 1), Integer(maximum), Integer(0), Integer(0)};
  EXPECT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, inclusive, result), ExecutionStatus::IntegerOverflow);
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeIntegerLoopTest, MatchesCheckedArithmeticAcrossSmallRangesAndIntegerEndpoints) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *INTEGER_LOOP_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::array<std::int64_t, 7> initials{minimum, minimum + 1, -7, 0, 7, maximum - 1, maximum};
  const std::array<std::int64_t, 5> terms{minimum, -3, 0, 3, maximum};
  for (std::int64_t operation = 0; operation < 6; ++operation) {
    for (std::int64_t bound = -8; bound <= 8; ++bound) {
      for (const auto initial : initials) {
        for (const auto term : terms) {
          const auto start = operation == 2 || operation == 5 ? -3 : 0;
          std::int64_t expected = initial;
          auto status = ExecutionStatus::Returned;
          for (std::int64_t index = start; operation == 1 ? index > bound : index < bound;
               index += operation == 1 ? -1 : 1) {
            const auto value = operation == 2 || operation == 4 ? term : index;
            const bool overflow = operation == 3 || operation == 4
                ? __builtin_sub_overflow(expected, value, &expected)
                : __builtin_add_overflow(expected, value, &expected);
            if (overflow) { status = ExecutionStatus::IntegerOverflow; break; }
          }
          const std::array arguments{Integer(operation), Integer(start), Integer(bound),
                                     Integer(initial), Integer(term)};
          NativeValue result;
          ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), status)
              << operation << ' ' << bound << ' ' << initial << ' ' << term;
          if (status == ExecutionStatus::Returned) {
            EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), expected);
          }
        }
      }
    }
  }
}
'''
