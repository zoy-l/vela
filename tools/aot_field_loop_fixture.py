"""Field loop transformations preserve aliases and exact arithmetic failure prefixes."""

from string import Template


_FLOAT_UPDATE = Template("""export func $name(values: FloatBox[], bound: int, initial: double): double {
  if (values.length != 5) { return -99.0; }
  let divisor = initial;
  let total = 0.0;
  for (let round = 0; round < bound; round++) {
    for (let index = 0; index < 4; index++) {
      const first = values[index];
      const second = values[index + 1];
      first.x += 1.0;
      second.x += first.x;
      first.y += second.x * 0.25;
      second.y += first.y;
      total += 1.0 / divisor;
$root_calculation      first.x += 3.0;
      second.x += 4.0;
      divisor -= 0.25;
    }
  }
  return total;
}
""")

SOURCE = """
class Box { value: int; }
export class FloatBox { x: double; y: double; }
export func floatPrepare(aliased: bool): FloatBox[] {
  const first = FloatBox(x: 1.0, y: 2.0);
  const second = aliased ? first : FloatBox(x: 3.0, y: 4.0);
  return [first, second, FloatBox(x: 5.0, y: 6.0),
          FloatBox(x: 7.0, y: 8.0), FloatBox(x: 9.0, y: 10.0)];
}
""" + _FLOAT_UPDATE.substitute(name="floatUpdate", root_calculation="") + _FLOAT_UPDATE.substitute(
    name="floatRootUpdate",
    root_calculation="      total += Math.sqrt(first.x * first.x + second.y * second.y);\n") + """\
let guardedValues: FloatBox[] = [];
export func guardedStatus(index: int): double { return guardedValues[index].y; }
export func guardedPrefix(initial: double): double {
  const values = [FloatBox(x: 1.0, y: 2.0), FloatBox(x: 3.0, y: 4.0),
                  FloatBox(x: 5.0, y: 6.0), FloatBox(x: 7.0, y: 8.0), FloatBox(x: 9.0, y: 10.0)];
  guardedValues = values;
  let divisor = initial;
  for (let round = 0; round < 2051; round++) {
    for (let i = 0; i < 5; i++) {
      const first = values[i];
      for (let j = i + 1; j < 5; j++) {
        const second = values[j];
        const dx = first.x - second.x;
        const root = Math.sqrt(dx * dx);
        first.y += 1.0;
        const amount = root / divisor;
        first.x += amount;
        second.x -= amount;
        second.y += 2.0;
        divisor -= 0.25;
      }
    }
  }
  return values[0].x;
}
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
#include <cmath>
extern "C" const NativeImage* FIELD_LOOP_IMAGE() noexcept;

class NativeFieldLoopTest : public NativeImageTest<FIELD_LOOP_IMAGE, 1> {
 protected:
  void ExpectFloatingFieldPrefixes(std::string_view function_name);
};

TEST_F(NativeFieldLoopTest, ExactResultsAndFailurePrefixesSurviveAliasingAndFinally) {
  NativeExecution execution(*instance);
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const std::array<std::array<std::int64_t, 4>, 13> cases{{
      {0, 7, -1, 7}, {0, maximum, 0, maximum}, {0, 0, 1000, 1000},
      {0, minimum, 2049, minimum + 2049}, {1, maximum, 2049, maximum - 2049},
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
    EXPECT_EQ(execution.current_frame(), nullptr);
    EXPECT_FALSE(execution.HasException());
  }
}

void NativeFieldLoopTest::ExpectFloatingFieldPrefixes(std::string_view function_name) {
  NativeExecution execution(*instance);
  auto& heap = isolate.heap();
  const auto prepare = instance->FindFunction("floatPrepare");
  const auto update = instance->FindFunction(function_name);
  ASSERT_TRUE(prepare);
  ASSERT_TRUE(update);
  const auto prepare_entry = image->function(*prepare).entry;
  const auto update_entry = image->function(*update).entry;
  const auto infinity = std::numeric_limits<double>::infinity();
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  const auto signaling_nan = std::numeric_limits<double>::signaling_NaN();
  const auto subnormal = std::numeric_limits<double>::denorm_min();
  for (const auto aliased : {false, true}) {
    for (const std::int64_t bound : {-1, 0, 1, 2, 3, 17, 128, 1025}) {
      for (const auto initial : {0.0, -0.0, 0.25, 0.5, 1.0, 1.25, 2.5, -3.0,
                                 2048.0, subnormal, -subnormal, 1e150, -1e150,
                                 infinity, -infinity, nan, signaling_nan}) {
        SCOPED_TRACE(aliased);
        SCOPED_TRACE(bound);
        SCOPED_TRACE(initial);
        const std::array arguments{NativeValue{.payload = aliased ? 1U : 0U,
                                               .kind = NativeValueKind::Boolean}};
        NativeValue result;
        ASSERT_EQ(Execute(prepare_entry, execution, arguments, result),
                  ExecutionStatus::Returned);
        Value values = DecodeNativeValue(result);
        ASSERT_TRUE(values.IsObject());
        const RootRange root(heap, &values, 1);
        std::array<std::array<double, 2>, 5> expected{{
            {1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}, {7.0, 8.0}, {9.0, 10.0},
        }};
        const auto last = heap.GetElement(*values.AsObject(), 4);
        ASSERT_TRUE(last.has_value());
        ASSERT_TRUE(heap.SetField(*last->AsObject(), 1, Value::Double(initial)));
        expected[4][1] = initial;
        const auto slot = [aliased](std::size_t index) {
          return aliased && index == 1 ? std::size_t{0} : index;
        };
        auto divisor = initial;
        auto total = 0.0;
        auto status = ExecutionStatus::Returned;
        for (std::int64_t round = 0; round < bound && status == ExecutionStatus::Returned; ++round) {
          for (std::size_t index = 0; index < 4; ++index) {
            auto& first = expected[slot(index)];
            auto& second = expected[slot(index + 1)];
            first[0] += 1.0;
            second[0] += first[0];
            first[1] += second[0] * 0.25;
            second[1] += first[1];
            if (divisor == 0.0) {
              status = ExecutionStatus::DivisionByZero;
              break;
            }
            total += 1.0 / divisor;
            if (function_name == "floatRootUpdate") {
              total += std::sqrt(first[0] * first[0] + second[1] * second[1]);
            }
            first[0] += 3.0;
            second[0] += 4.0;
            divisor -= 0.25;
          }
        }
        const std::array input{EncodeNativeValue(values), Integer(bound), Double(initial)};
        ASSERT_EQ(Execute(update_entry, execution, input, result), status);
        if (status == ExecutionStatus::Returned) {
          const auto actual = std::bit_cast<double>(result.payload);
          if (std::isnan(total)) EXPECT_TRUE(std::isnan(actual));
          else EXPECT_EQ(std::bit_cast<std::uint64_t>(actual), std::bit_cast<std::uint64_t>(total));
          EXPECT_FALSE(execution.HasException());
        } else {
          ASSERT_NO_FATAL_FAILURE(ExpectPendingException(isolate, execution, status, "division by zero"));
          const auto& trace = execution.ExceptionDiagnostic().stack_trace;
          ASSERT_FALSE(trace.empty());
          EXPECT_EQ(trace.front().function, function_name);
        }
        for (std::size_t index = 0; index < expected.size(); ++index) {
          const auto item = heap.GetElement(*values.AsObject(), static_cast<std::int64_t>(index));
          ASSERT_TRUE(item.has_value());
          for (std::uint32_t field = 0; field < 2; ++field) {
            const auto actual = Heap::GetField(*item->AsObject(), field);
            ASSERT_TRUE(actual.has_value());
            ASSERT_TRUE(actual->IsDouble());
            EXPECT_EQ(std::bit_cast<std::uint64_t>(actual->AsDouble()),
                      std::bit_cast<std::uint64_t>(expected[slot(index)][field]));
          }
        }
        EXPECT_EQ(execution.current_frame(), nullptr);
      }
    }
  }
}

TEST_F(NativeFieldLoopTest, FloatingPrefixesPreserveAliasesAndEveryFailurePositionUnderGc) {
  ASSERT_NO_FATAL_FAILURE(ExpectFloatingFieldPrefixes("floatUpdate"));
}

TEST_F(NativeFieldLoopTest, FloatingRootPrefixesPreserveBitsAndFailuresUnderGc) {
  ASSERT_NO_FATAL_FAILURE(ExpectFloatingFieldPrefixes("floatRootUpdate"));
}

TEST_F(NativeFieldLoopTest, BatchedDivisionFailureCommitsTheExactPrefixOfTheCurrentIteration) {
  const auto update = instance->FindFunction("guardedPrefix");
  const auto read = instance->FindFunction("guardedStatus");
  ASSERT_TRUE(update);
  ASSERT_TRUE(read);
  const auto update_entry = image->function(*update).entry;
  const auto read_entry = image->function(*read).entry;
  struct Failure final {
    double initial;
    std::array<double, 5> fields;
  };
  // 第三轮和跨分块后的第三对失败，负零则在第一对失败。
  for (const auto& [initial, expected] : std::array{
           Failure{5.5, {13.0, 16.0, 20.0, 22.0, 26.0}},
           Failure{2750.5, {4405.0, 5506.0, 6608.0, 7708.0, 8810.0}},
           Failure{-0.0, {3.0, 4.0, 6.0, 8.0, 10.0}},
       }) {
    SCOPED_TRACE(initial);
    {
      NativeExecution execution(*instance);
      const std::array arguments{Double(initial)};
      NativeValue output;
      ASSERT_EQ(Execute(update_entry, execution, arguments, output), ExecutionStatus::DivisionByZero);
      ASSERT_NO_FATAL_FAILURE(ExpectPendingException(isolate, execution, ExecutionStatus::DivisionByZero,
                                                     "division by zero"));
      const auto& trace = execution.ExceptionDiagnostic().stack_trace;
      ASSERT_FALSE(trace.empty());
      EXPECT_EQ(trace.front().function, "guardedPrefix");
    }
    NativeExecution execution(*instance);
    for (std::size_t index = 0; index < expected.size(); ++index) {
      const std::array arguments{Integer(static_cast<std::int64_t>(index))};
      NativeValue output;
      ASSERT_EQ(Execute(read_entry, execution, arguments, output), ExecutionStatus::Returned);
      ASSERT_EQ(output.kind, NativeValueKind::Double);
      EXPECT_EQ(std::bit_cast<double>(output.payload), expected[index]);
    }
  }
}
'''
