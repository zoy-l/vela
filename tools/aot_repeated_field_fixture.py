"""Repeated object traversals preserve aliases, FP order, state and error prefixes."""

SOURCE = """
class Point { x: double; dx: double; }
let points: Point[] = [];
let stable: Point[] = [];
func repeat(): void {
  for (let round = 0; round < 10; round++) {
    for (const point in points) { point.x += point.dx; }
  }
}
func repeatStable(): void {
  for (let round = 0; round < 10; round++) {
    for (const point in stable) { point.x += point.dx; }
  }
}
func fail(): void {
  for (let round = 0; round < 10; round++) {
    for (let index = 0; index <= points.length; index++) {
      const point = points[index]; point.x += point.dx;
    }
  }
}
export func calculate(operation: int, initial: double, delta: double): double {
  const point = Point(x: initial, dx: delta);
  const other = Point(x: -initial, dx: -delta);
  points = [point, point, other];
  if (operation == 0) { repeat(); }
  if (operation == 1) {
    for (let round = 0; round < 10; round++) {
      for (const item in points) { item.x += item.dx; item.dx += 1.0; }
    }
  }
  if (operation == 2) {
    for (let round = 0; round < 10; round++) {
      for (const item in points) { item.x += item.dx; item.dx += item.x; }
    }
  }
  if (operation == 3) {
    try { fail(); } catch (error: Error) {
      if (error.code != 'IndexOutOfBounds') { return -99.0; }
    }
  }
  if (operation == 4) {
    let enabled = false;
    for (let round = 0; round < 10; round++) {
      if (enabled) { for (const item in points) { item.x += item.dx; } }
      enabled = !enabled;
    }
  }
  if (operation == 5) {
    let total = 0;
    for (let round = 0; round < 10; round++) {
      total++;
      for (const item in points) { item.x += item.dx; }
    }
    return point.x + other.x + total.toDouble();
  }
  if (operation == 6) {
    stable.clear(); stable.push(point); stable.push(point); stable.push(other);
    repeatStable();
  }
  return point.x + other.x;
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* REPEATED_FIELD_IMAGE() noexcept;
using NativeRepeatedFieldTest = NativeImageTest<REPEATED_FIELD_IMAGE, 1>;

TEST_F(NativeRepeatedFieldTest, PreservesRepeatedAliasesFpOrderAndFailurePrefixes) {
  NativeExecution execution(*instance);
  for (const auto initial : {0.0, 1.0e16, -1.0e16, 1.5}) {
    for (std::int64_t operation = 0; operation <= 6; ++operation) {
      SCOPED_TRACE(::testing::PrintToString(std::pair(initial, operation)));
      double first = initial;
      double other = -initial;
      double delta = 1.0;
      double other_delta = -1.0;
      for (std::int64_t round = 0; round < (operation == 3 ? 1 : 10); ++round) {
        if (operation == 4 && round % 2 == 0) { continue; }
        for (auto* value : {&first, &first, &other}) {
          auto& step = value == &first ? delta : other_delta;
          *value += step;
          if (operation == 1) { step += 1.0; }
          if (operation == 2) { step += *value; }
        }
      }
      const auto expected = first + other + (operation == 5 ? 10.0 : 0.0);
      const std::array arguments{Integer(operation), Double(initial), Double(1.0)};
      NativeValue output;
      ASSERT_EQ(Execute(REPEATED_FIELD_ENTRY, execution, arguments, output),
                ExecutionStatus::Returned);
      EXPECT_EQ(output.payload, std::bit_cast<std::uint64_t>(expected));
      EXPECT_EQ(execution.current_frame(), nullptr);
      EXPECT_FALSE(execution.HasException());
    }
  }
}
'''
