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
let boundedValues: int[] = [];
let boundedAlias = boundedValues;
let unboundedValues: int[] = [];
func increment(value: int): int { return value + 1; }
func prepareBounded(count: int, seed: int): void {
  boundedAlias.clear();
  boundedAlias.reserve(count);
  for (let index = 0; index < count; index++) { boundedAlias.push((seed ^ index) & 1023); }
}
func boundedSum(): int {
  const values = boundedValues;
  const count = values.length;
  let sum = 0;
  for (let index = 0; index < count; index++) { sum += increment(values[index]); }
  return sum;
}
func arrayPrefix(count: int, initial: int, term: int): int {
  unboundedValues = [1, term, 0];
  let sum = initial;
  effects = 0;
  try {
    for (let index = 0; index < count; index++) {
      effects = index;
      sum += increment(unboundedValues[index]);
    }
  } catch (error: Error) { return sum; }
  return sum;
}
class CallbackCounter {
  value: int = -1;
  accept(value: int): int { this.value = value; return value; }
}
const callbackCounter = CallbackCounter();
export func assignArray(values: int[], initial: int): int {
  let sum = initial;
  for (let index = 0; index < values.length; index++) {
    callbackCounter.value = values[index];
    sum += callbackCounter.value;
  }
  return sum;
}
export func callArray(values: int[], initial: int): int {
  let sum = initial;
  for (let index = 0; index < values.length; index++) {
    sum += callbackCounter.accept(values[index]);
  }
  return sum;
}
export func writeArray(values: int[], value: int): void { values[0] = value; }
export func appendArray(values: int[], value: int): void { values.push(value); }
export func rootedArray(values: int[]): int {
  const text = values[0].toString();
  return values[0] + values[1] + text.length - 2;
}
func callbackSum(count: int, initial: int): int {
  const values: int[] = [];
  for (let index = 0; index < count; index++) { values.push(index); }
  callbackCounter.value = -1;
  let sum = initial;
  for (let index = 0; index < values.length; index++) {
    sum += callbackCounter.accept(values[index]);
  }
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
  if (operation == 10) { prepareBounded(bound, term); return boundedSum(); }
  if (operation == 11) { return arrayPrefix(bound, initial, term); }
  if (operation == 12) { return effects; }
  if (operation == 13) { return callbackSum(bound, initial); }
  if (operation == 14) { return callbackCounter.value; }
  return 0;
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* INTEGER_LOOP_IMAGE() noexcept;
using NativeIntegerLoopTest = NativeImageTest<INTEGER_LOOP_IMAGE>;
using NativeIntegerLoopGcTest = NativeImageTest<INTEGER_LOOP_IMAGE, 1>;

const std::array kObserveAssignedField{Integer(14), Integer(0), Integer(0), Integer(0), Integer(0)};

std::array<NativeValue, 5> IntegerLoopArguments(std::span<const std::int64_t> values) {
  return {Integer(values[0]), Integer(values[1]), Integer(values[2]),
          Integer(values[3]), Integer(values[4])};
}

void ExpectArrayOverflowTrace(NativeExecution& execution, std::string_view name) {
  ASSERT_TRUE(execution.HasException());
  const auto& trace = execution.ExceptionDiagnostic().stack_trace;
  ASSERT_FALSE(trace.empty());
  EXPECT_EQ(trace.front().function, name);
}

void ExpectAssignedField(NativeExecution& execution, std::int64_t expected) {
  NativeValue result;
  ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, kObserveAssignedField, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), expected);
  EXPECT_EQ(execution.current_frame(), nullptr);
  EXPECT_FALSE(execution.HasException());
}

void ExpectIntegerLoopCases(NativeExecution& execution,
                            std::span<const std::array<std::int64_t, 6>> cases) {
  for (const auto& item : cases) {
    SCOPED_TRACE(::testing::PrintToString(item));
    const auto arguments = IntegerLoopArguments(item);
    NativeValue result;
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), item[5]);
    EXPECT_EQ(execution.current_frame(), nullptr);
    EXPECT_FALSE(execution.HasException());
  }
}

TEST_F(NativeIntegerLoopTest, DynamicBoundsAndWideIntermediatesKeepExactResults) {
  NativeExecution execution(*instance);
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
      // 极值附近跨越多个轮询片段；正常完成不依赖全跨度循环被闭式折叠。
      {2, minimum, minimum + 2049, 42, 0, 42},
      {2, minimum, minimum + 2049, minimum, 1, minimum + 2049},
      {2, maximum - 2049, maximum, maximum, -1, maximum - 2049},
      {2, 0, 1, maximum, minimum, -1},
      {3, 0, 4, 0, 0, -6}, {4, 0, 1, minimum, minimum, 0},
      {5, -4, 5, 17, 0, 17}, {9, 0, 4, maximum - 2, 0, 49},
  }};
  ExpectIntegerLoopCases(execution, cases);
}

TEST_F(NativeIntegerLoopGcTest, OverflowKeepsItsStatusAndOriginalSourcePosition) {
  NativeExecution execution(*instance);
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::array<std::array<std::int64_t, 5>, 8> cases{{
      {0, 0, 4294967297LL, 0, 0}, {0, 0, maximum, 0, 0},
      {1, 0, -4294967297LL, 0, 0}, {1, 0, minimum, 0, 0},
      {2, minimum, maximum, 0, maximum}, {3, 0, 3, minimum, 0},
      {4, 0, 1, 0, minimum}, {4, 0, 2, minimum, maximum},
  }};
  const std::array names{"ascending", "descending", "constant", "subtract", "subtractConstant"};
  const std::array<std::uint32_t, 5> lines{INTEGER_LOOP_TRACE_LINES};
  for (const auto& item : cases) {
    SCOPED_TRACE(::testing::PrintToString(item));
    const auto arguments = IntegerLoopArguments(item);
    NativeValue result;
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), ExecutionStatus::IntegerOverflow);
    ASSERT_NO_FATAL_FAILURE(ExpectPendingException(isolate, execution, ExecutionStatus::IntegerOverflow,
                                                  "integer overflow"));
    const auto diagnostic = execution.ExceptionDiagnostic();
    const auto& trace = diagnostic.stack_trace;
    ASSERT_FALSE(trace.empty());
    const auto operation = static_cast<std::size_t>(item[0]);
    EXPECT_EQ(trace.front().function, names[operation]);
    EXPECT_EQ(trace.front().line, lines[operation]);
  }
}

TEST_F(NativeIntegerLoopGcTest, ObservedPrefixesAndOtherFailureExitsRemainOrdered) {
  NativeExecution execution(*instance);
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::array<std::array<std::int64_t, 6>, 2> cases{{
      {6, 0, 4, maximum - 2, 0, maximum - 1}, {7, 0, 4, maximum - 2, 0, 3},
  }};
  ExpectIntegerLoopCases(execution, cases);
  const std::array crossing{Integer(5), Integer(-4), Integer(5), Integer(minimum + 2), Integer(0)};
  NativeValue result;
  EXPECT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, crossing, result), ExecutionStatus::IntegerOverflow);
  const std::array inclusive{Integer(8), Integer(maximum - 1), Integer(maximum), Integer(0), Integer(0)};
  EXPECT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, inclusive, result), ExecutionStatus::IntegerOverflow);
  EXPECT_EQ(execution.current_frame(), nullptr);
}

TEST_F(NativeIntegerLoopTest, MatchesCheckedArithmeticAcrossSmallRangesAndIntegerEndpoints) {
  NativeExecution execution(*instance);
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

TEST_F(NativeIntegerLoopGcTest, ClosedArrayRangesHoldAcrossCallsAliasesEmptyAndChunkBoundaries) {
  NativeExecution execution(*instance);
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  for (const std::int64_t count : {0, 1, 1000, 1024, 1025, 4097}) {
    for (const auto seed : std::array<std::int64_t, 3>{-1, 42, maximum}) {
      std::int64_t expected = 0;
      for (std::int64_t index = 0; index < count; ++index) {
        expected += ((seed ^ index) & 1023) + 1;
      }
      const std::array arguments{Integer(10), Integer(0), Integer(count), Integer(0), Integer(seed)};
      NativeValue result;
      ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
      EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), expected) << count << ' ' << seed;
      EXPECT_EQ(execution.current_frame(), nullptr);
      EXPECT_FALSE(execution.HasException());
    }
  }
}

TEST_F(NativeIntegerLoopGcTest, RotatedCallsKeepBudgetBoundariesAndTheOverflowingWrite) {
  NativeExecution execution(*instance);
  NativeValue result;
  for (const std::int64_t count : {0, 1, 2, 127, 128, 1023, 1024, 1025, 2049}) {
    const std::array arguments{Integer(13), Integer(0), Integer(count), Integer(0), Integer(0)};
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), count * (count - 1) / 2);
    ASSERT_NO_FATAL_FAILURE(ExpectAssignedField(execution, count - 1));
  }
  const auto initial = std::numeric_limits<std::int64_t>::max() - 2;
  for (const std::int64_t count : {3, 128, 1024, 1025}) {
    const std::array arguments{Integer(13), Integer(0), Integer(count), Integer(initial), Integer(0)};
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), ExecutionStatus::IntegerOverflow);
    ASSERT_NO_FATAL_FAILURE(ExpectPendingException(isolate, execution, ExecutionStatus::IntegerOverflow,
                                                  "integer overflow"));
    const auto diagnostic = execution.ExceptionDiagnostic();
    ASSERT_FALSE(diagnostic.stack_trace.empty());
    EXPECT_EQ(diagnostic.stack_trace.front().function, "callbackSum");
    ASSERT_NO_FATAL_FAILURE(ExpectAssignedField(execution, 2));
  }
}

TEST_F(NativeIntegerLoopGcTest, ParameterArrayTrialsPreserveEmptySignedAndOverflowingWrites) {
  NativeExecution execution(*instance);
  for (const auto name : {"assignArray", "callArray"}) {
    SCOPED_TRACE(name);
    const auto function = instance->FindFunction(name);
    ASSERT_TRUE(function);
    const auto& description = instance->image().function(*function);
    const auto parameters = description.parameters.values();
    ASSERT_EQ(parameters.size(), 2U);
    const auto* layout = instance->layouts().types[parameters.front().type].array;
    ASSERT_NE(layout, nullptr);
    auto& heap = isolate.heap();
    NativeValue previous_result;
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, kObserveAssignedField, previous_result), ExecutionStatus::Returned);
    auto previous = std::bit_cast<std::int64_t>(previous_result.payload);
    const auto check = [&](std::span<const std::int64_t> values, std::int64_t initial) {
      auto* array = heap.AllocateArray(*layout, values.size());
      ASSERT_NE(array, nullptr);
      Value retained = Value::Object(array);
      const RootRange root(heap, &retained, 1);
      auto expected = initial;
      auto assigned = previous;
      auto status = ExecutionStatus::Returned;
      for (std::size_t index = 0; index < values.size(); ++index) {
        ASSERT_TRUE(heap.SetElement(*array, static_cast<std::uint32_t>(index),
                                    Value::Integer(values[index])));
      }
      for (const auto value : values) {
        assigned = value;
        if (__builtin_add_overflow(expected, value, &expected)) {
          status = ExecutionStatus::IntegerOverflow;
          break;
        }
      }
      const std::array arguments{EncodeNativeValue(retained), Integer(initial)};
      NativeValue result;
      ASSERT_EQ(Execute(description.entry, execution, arguments, result), status);
      if (status == ExecutionStatus::Returned) {
        EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), expected);
        EXPECT_FALSE(execution.HasException());
      } else {
        ASSERT_NO_FATAL_FAILURE(ExpectArrayOverflowTrace(execution, name));
      }
      ASSERT_NO_FATAL_FAILURE(ExpectAssignedField(execution, assigned));
      previous = assigned;
    };
    std::array<std::int64_t, 2049> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
      values[index] = static_cast<std::int64_t>(index & 1023);
    }
    for (const std::size_t count : {0, 1, 2, 7, 8, 9, 127, 128, 129, 1023, 1024, 1025, 2049}) {
      SCOPED_TRACE(count);
      ASSERT_NO_FATAL_FAILURE(check(std::span(values).first(count), 0));
    }
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const std::array<std::array<std::int64_t, 3>, 4> endpoints{{
        {-1, 3, -2}, {maximum, -maximum, 7}, {1, maximum, 7}, {minimum, -1, 7},
    }};
    for (const auto initial : std::array<std::int64_t, 3>{0, -7, maximum - 2}) {
      for (const auto& items : endpoints) {
        SCOPED_TRACE(::testing::PrintToString(items));
        SCOPED_TRACE(initial);
        ASSERT_NO_FATAL_FAILURE(check(items, initial));
      }
    }
  }
}

TEST_F(NativeIntegerLoopGcTest, ParameterArrayRangesInvalidateAtEveryMutableBoundary) {
  const auto selected = instance->FindFunction("assignArray");
  ASSERT_TRUE(selected);
  const auto& description = instance->image().function(*selected);
  const auto* layout = instance->layouts().types[description.parameters.values().front().type].array;
  ASSERT_NE(layout, nullptr);
  auto& heap = isolate.heap();
  constexpr std::size_t kCount = 128;
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto expect_overflow = [&](Value input, std::int64_t initial, std::string_view name) {
    const auto function = instance->FindFunction(name);
    ASSERT_TRUE(function);
    NativeExecution execution(*instance);
    const std::array arguments{EncodeNativeValue(input), Integer(initial)};
    NativeValue result;
    ASSERT_EQ(Execute(instance->image().function(*function).entry, execution, arguments, result),
              ExecutionStatus::IntegerOverflow);
    ASSERT_NO_FATAL_FAILURE(ExpectArrayOverflowTrace(execution, name));
    ASSERT_NO_FATAL_FAILURE(ExpectAssignedField(execution, maximum));
  };
  for (const auto name : {"writeArray", "appendArray"}) {
    SCOPED_TRACE(name);
    Value input = Value::Object(heap.AllocateArray(*layout, kCount));
    ASSERT_NE(input.AsObject(), nullptr);
    const RootRange root(heap, &input, 1);
    ASSERT_TRUE(heap.ReserveArray(*input.AsObject(), kCount * 2));
    const auto function = instance->FindFunction(name);
    ASSERT_TRUE(function);
    {
      NativeExecution execution(*instance);
      const std::array arguments{EncodeNativeValue(input), Integer(maximum)};
      NativeValue result;
      ASSERT_EQ(Execute(instance->image().function(*function).entry, execution, arguments, result),
                ExecutionStatus::Returned);
    }
    for (const auto reader : {"assignArray", "callArray"}) {
      ASSERT_NO_FATAL_FAILURE(expect_overflow(input, 1, reader));
    }
  }
  Value input = Value::Object(heap.AllocateArray(*layout, kCount));
  ASSERT_NE(input.AsObject(), nullptr);
  const RootRange root(heap, &input, 1);
  const auto elements = Heap::IntegerElements(*input.AsObject());
  ASSERT_TRUE(elements);
  ASSERT_TRUE(heap.FillArray(*input.AsObject(), Value::Integer(0), 0, kCount));
  elements->front() = 1;
  elements->back() = maximum;
  for (const auto name : {"assignArray", "callArray"}) {
    ASSERT_NO_FATAL_FAILURE(expect_overflow(input, 0, name));
  }
}

TEST_F(NativeIntegerLoopGcTest, PublicCapturedArrayEntryPreservesScopesErrorsAndOwnership) {
  using ArrayEntry = vela::Entry<std::int64_t(vela::Array<std::int64_t>)>;
  const auto bound = ArrayEntry::Bind(*instance, "rootedArray");
  ASSERT_TRUE(bound);
  const auto captured = bound->Capture();
  ASSERT_TRUE(captured);
  instance.reset();
  isolate.heap().Collect();
  vela::HandleScope scope(isolate);
  const auto input = vela::CreateArray<std::int64_t>(scope);
  ASSERT_TRUE(input);
  ASSERT_TRUE(input->Push(scope, 40));
  ASSERT_TRUE(input->Push(scope, 2));
  for (int index = 0; index < 3; ++index) {
    isolate.heap().Collect();
    const auto result = captured->Call(scope, *input);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, 42);
    EXPECT_EQ(isolate.native_control().state().host_execution, nullptr);
  }
  const auto empty = vela::CreateArray<std::int64_t>(scope);
  ASSERT_TRUE(empty);
  const auto rejected = captured->Call(scope, *empty);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, ExecutionStatus::IndexOutOfBounds);
  ASSERT_FALSE(rejected.error().stack_trace.empty());
  EXPECT_EQ(rejected.error().stack_trace.front().function, "rootedArray");
  Isolate foreign;
  vela::HandleScope other(foreign);
  const auto wrong_scope = captured->Call(other, *input);
  ASSERT_FALSE(wrong_scope);
  EXPECT_EQ(wrong_scope.error().status, ExecutionStatus::InvalidIsolate);
}

TEST_F(NativeIntegerLoopGcTest, UnknownGlobalWritesKeepOverflowAndBoundsFailurePrefixes) {
  NativeExecution execution(*instance);
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::array<std::array<std::int64_t, 5>, 5> cases{{
      {3, 0, maximum, 2, 1}, {3, maximum - 2, 0, maximum, 1},
      {4, 0, 0, 4, 3}, {0, 42, maximum, 42, 0}, {3, minimum, minimum, minimum + 2, 1},
  }};
  for (const auto& item : cases) {
    SCOPED_TRACE(::testing::PrintToString(item));
    const std::array arguments{Integer(11), Integer(0), Integer(item[0]), Integer(item[1]), Integer(item[2])};
    NativeValue result;
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), item[3]);
    const std::array observed{Integer(12), Integer(0), Integer(0), Integer(0), Integer(0)};
    ASSERT_EQ(Execute(INTEGER_LOOP_ENTRY, execution, observed, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), item[4]);
    EXPECT_EQ(execution.current_frame(), nullptr);
    EXPECT_FALSE(execution.HasException());
  }
}
'''
