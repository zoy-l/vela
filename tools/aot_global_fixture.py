"""Direct global reads preserve typed storage, GC and instance switching."""

SOURCE = """
external func collect(): void;
class Box { public param value: int; }
enum Choice { first, second }
let enabled = true;
let fraction = 1.5;
let optional: int? = null;
let text = 'ready';
let erased: dynamic = 42;
let item: Box? = Box(value: 7);
let choice = Choice.second;
export func calculate(operation: int): int {
  if (operation == 0) {
    const label = text;
    const box = item;
    collect();
    if (enabled && fraction == 1.5 && optional == null && erased == 42 &&
        label == 'ready' && box?.value == 7 && choice == Choice.second) { return 42; }
    return 0;
  }
  if (operation == 1) {
    enabled = false;
    fraction = -2.5;
    optional = 41;
    text = 'changed';
    erased = text;
    item = Box(value: 8);
    choice = Choice.first;
  }
  if (operation == 1 || operation == 2) {
    collect();
    if (!enabled && fraction == -2.5 && optional == 41 && erased == 'changed' &&
        text == 'changed' && item?.value == 8 && choice == Choice.first) { return 42; }
    return 0;
  }
  optional = null;
  erased = null;
  item = null;
  collect();
  if (optional == null && erased == null && item == null) { return 42; }
  return 0;
}
"""

LEAF_SOURCE = """
let divisor = 0;
let answer = 84 / divisor;
let cycle = readCycle();
func readCycle(): int { return cycle; }
export func repair(): void { divisor = 2; }
export func calculate(operation: int): int {
  if (operation == 0) { return answer; }
  if (operation == 1) { return divisor; }
  return cycle;
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* GLOBAL_STORAGE_IMAGE() noexcept;
extern "C" const NativeImage* GLOBAL_LEAF_IMAGE() noexcept;

TEST(NativeGlobalTest, ReadyLoadsObserveMutationsNullsAndManagedValuesAcrossCollection) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  const auto instance = NativeInstance::Instantiate(isolate, *GLOBAL_STORAGE_IMAGE());
  const auto second = NativeInstance::Instantiate(isolate, *GLOBAL_STORAGE_IMAGE());
  ASSERT_TRUE(instance.has_value());
  ASSERT_TRUE(second.has_value());
  NativeExecution execution(**instance);
  NativeExecution other(**second);
  NativeValue output;
  for (const auto operation : {0, 0, 1, 2, 2, 3, 3}) {
    const std::array arguments{Integer(operation)};
    ASSERT_EQ(Execute(GLOBAL_STORAGE_ENTRY, execution, arguments, output), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
  const std::array original{Integer(0)};
  ASSERT_EQ(Execute(GLOBAL_STORAGE_ENTRY, other, original, output), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 42);
  {
    const NativeInstanceScope scope(execution, **second);
    ASSERT_EQ(Execute(GLOBAL_STORAGE_ENTRY, execution, original, output), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 42);
  }
  const std::array changed{Integer(3)};
  ASSERT_EQ(Execute(GLOBAL_STORAGE_ENTRY, execution, changed, output), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 42);
}

TEST(NativeGlobalTest, ScalarLazyInitializationKeepsFailureTracesRetryAndDepthLimits) {
  Isolate isolate(1);
  const auto* image = GLOBAL_LEAF_IMAGE();
  const auto instance = NativeInstance::Instantiate(isolate, *image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  NativeValue output;
  const std::array answer{Integer(0)};
  ASSERT_EQ(Execute(GLOBAL_LEAF_ENTRY, execution, answer, output), ExecutionStatus::DivisionByZero);
  EXPECT_EQ(execution.frame_count(), 0U);
  const auto diagnostic = execution.ExceptionDiagnostic();
  ASSERT_EQ(diagnostic.stack_trace.size(), 2U);
  EXPECT_EQ(diagnostic.stack_trace.back().function, "calculate");
  EXPECT_EQ(diagnostic.stack_trace.back().line, 8U);
  const auto functions = image->functions.values();
  const auto repair = std::ranges::find(functions, "repair", [](const auto& function) {
    return function.name.text();
  });
  ASSERT_NE(repair, functions.end());
  ASSERT_EQ(Execute(repair->entry, execution, {}, output), ExecutionStatus::Returned);
  EXPECT_FALSE(execution.HasException());
  for (const auto operation : {0, 0, 2, 2}) {
    const std::array arguments{Integer(operation)};
    const auto expected = operation == 0 ? ExecutionStatus::Returned
                                          : ExecutionStatus::CyclicInitialization;
    ASSERT_EQ(Execute(GLOBAL_LEAF_ENTRY, execution, arguments, output), expected);
    if (expected == ExecutionStatus::Returned) {
      EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 42);
    } else {
      EXPECT_EQ(execution.ExceptionDiagnostic().stack_trace.back().function, "calculate");
    }
    EXPECT_EQ(execution.frame_count(), 0U);
  }
  const auto second = NativeInstance::Instantiate(isolate, *image);
  ASSERT_TRUE(second.has_value());
  NativeExecution nested(**second);
  std::array<NativeTraceFrame, vela::kMaxCallFrames - 1> frames;
  const auto calculate = std::ranges::find(functions, "calculate", [](const auto& function) {
    return function.name.text();
  });
  ASSERT_NE(calculate, functions.end());
  const auto function_index = static_cast<std::uint32_t>(calculate - functions.begin());
  for (auto& frame : frames) {
    ASSERT_EQ(nested.Enter(frame, function_index), ExecutionStatus::Returned);
  }
  const std::array divisor{Integer(1)};
  EXPECT_EQ(Execute(GLOBAL_LEAF_ENTRY, nested, divisor, output), ExecutionStatus::StackOverflow);
  EXPECT_EQ(nested.frame_count(), frames.size());
  while (nested.frame_count() != 0) { nested.Exit(); }
  ASSERT_EQ(Execute(GLOBAL_LEAF_ENTRY, nested, divisor, output), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 0);
  EXPECT_FALSE(nested.HasException());
}
'''
