"""Exercise direct and checked host ABIs against the same emitted AOT image."""

SOURCE = r'''
external func step(value: int): int = 'binding.step';
external func invert(value: bool): bool = 'binding.invert';
external func measure(text: string): int = 'binding.measure';
external func combine(value: int, scale: double, enabled: bool): double = 'binding.combine';
external func collect(): void = 'binding.collect';
external func scale(value: double): double = 'binding.scale';
external func noop(): void = 'binding.noop';
external func scoped(): int = 'binding.scoped';
external func mixed(left: string, value: int, enabled: bool, scale: double, right: string): int = 'binding.mixed';
external func requestStop(): void = 'binding.stop';
let late = initializeLate();
func initializeLate(): int { noop(); return 42; }
func borrowedFailure(value: int): int { noop(); return value + 1; }
func borrowedLazy(): int { noop(); return late; }
func borrowedStop(): int { requestStop(); return 42; }

interface HostOperation { apply<T extends int>(value: T): int; }
class StepOperation implements HostOperation {
  apply<T extends int>(value: T): int { return step(value); }
}

export func calculate(operation: int): int {
  if (operation == 0) {
    let result = 0;
    for (let index = 0; index < 1000; index++) { result += step(index); }
    return result;
  }
  if (operation == 1) {
    noop();
    if (invert(false) && !invert(true) && combine(20, 2.1, true) == 42.0 && scale(21.0) == 42.0) { return 42; }
    return 0;
  }
  if (operation == 2) {
    const text = 'rooted:${operation}';
    collect();
    const length = measure(text);
    return length + text.length;
  }
  if (operation == 4) {
    const bridge: HostOperation = StepOperation();
    const invoke: (int) => int = bridge.apply;
    return invoke(41);
  }
  if (operation == 5) { return scoped() + scoped(); }
  if (operation == 6) {
    let result = 0;
    for (let index = 0; index < 8; index++) {
      result += mixed('left', 20, true, 2.1, 'right');
    }
    return result;
  }
  if (operation == 7) { return manyViews(); }
  if (operation == 8) { return borrowedFailure(9223372036854775807); }
  if (operation == 9) { return borrowedFailure(41); }
  if (operation == 10) { return borrowedStop(); }
  if (operation == 11) { return borrowedLazy(); }
  try { return step(-1); }
  catch (error: Error) {
    if (error.code == 'NativeFailure' && error.message == 'negative input') { return 42; }
    return -1;
  }
}
'''

SOURCE += "func manyViews(): int { let sum = 0;\n" + "\n".join(
    f"sum += measure('v{index}');" for index in range(200)) + "\nreturn sum; }\n"

HOST_TESTS = r'''
extern "C" const NativeImage* BINDING_IMAGE() noexcept;
namespace binding_test {

std::int64_t Step(std::int64_t value) noexcept { return value + 1; }
std::expected<std::int64_t, vela::Error> CheckedStep(std::int64_t value) noexcept {
  if (value < 0) { return std::unexpected(vela::Error{.message = "negative input", .stack_trace = {}}); }
  return value + 1;
}
bool Invert(bool value) noexcept { return !value; }
double Scale(double value) noexcept { return value * 2.0; }
void Noop() noexcept {}
void RequestStop(NativeContext& context) noexcept { context.isolate().native_control().RequestStop(); }
double Combine(NativeContext&, std::int64_t value, double scale, bool enabled) noexcept {
  return enabled ? static_cast<double>(value) * scale : 0.0;
}
std::int64_t Measure(NativeContext& context, std::string_view text) noexcept {
  context.isolate().heap().Collect();
  return static_cast<std::int64_t>(text.size());
}
std::int64_t Scoped(NativeContext& context) noexcept {
  auto text = vela::CreateString(context.handles(), "retained by direct host scope");
  if (!text) { return -1; }
  context.isolate().heap().Collect();
  return text->Utf8() == "retained by direct host scope" ? 21 : 0;
}

std::int64_t Mixed(NativeContext& context, std::string_view left, std::int64_t value,
                   bool enabled, double scale, std::string_view right) noexcept {
  auto* parent = context.isolate().native_control().state().host_execution;
  if (parent == nullptr) { return -1; }
  const auto trace = parent->CaptureTrace();
  if (trace.size() != 2 || trace.front().function != "mixed" ||
      trace.back().function != "calculate") { return -2; }
  auto nested = vela::Entry<std::int64_t(std::int64_t)>::Bind(parent->instance(), "calculate");
  if (!nested) { return -3; }
  const auto result = nested->Call(context.handles(), std::int64_t{1});
  if (!result || *result != 42 || parent != context.isolate().native_control().state().host_execution) {
    return -4;
  }
  context.isolate().heap().Collect();
  const auto valid = left == "left" && right == "right" && enabled &&
                     static_cast<double>(value) * scale == 42.0;
  // 强制改写 C++ 值参数，验证后续调用拿到独立副本，不能共享被宿主写坏的缓存视图。
  auto* bytes = reinterpret_cast<volatile unsigned char*>(&left);
  for (std::size_t index = 0; index < sizeof(left); ++index) { bytes[index] = 0; }
  return valid ? 42 : -5;
}

template <auto StepFunction>
void CheckBindings(std::int64_t failure_result) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<StepFunction>("binding.step")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Invert>("binding.invert")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Measure>("binding.measure")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Combine>("binding.combine")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("binding.collect")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Scale>("binding.scale")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Noop>("binding.noop")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Scoped>("binding.scoped")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Mixed>("binding.mixed")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<RequestStop>("binding.stop")));
  const auto instance = NativeInstance::Instantiate(isolate, *BINDING_IMAGE());
  ASSERT_TRUE(instance.has_value()) << instance.error().message;
  NativeExecution execution(**instance);
  const std::array<std::int64_t, 8> expected{500500, 42, 16, failure_result, 42, 42, 336, 690};
  for (std::size_t operation = 0; operation < expected.size(); ++operation) {
    const std::array arguments{Integer(static_cast<std::int64_t>(operation))};
    NativeValue result;
    ASSERT_EQ(Execute(BINDING_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), expected.at(operation)) << operation;
    EXPECT_EQ(execution.current_frame(), nullptr);
    EXPECT_FALSE(execution.HasException());
    EXPECT_EQ(isolate.native_control().state().host_execution, nullptr);
  }
  const auto blockers = isolate.ReloadBlockers();
  EXPECT_EQ(std::ranges::count(blockers, ExternalResourceKind::RootRange,
                             &ExternalResourceInfo::kind), 0);
  NativeValue output;
  const std::array overflow{Integer(8)};
  ASSERT_EQ(Execute(BINDING_ENTRY, execution, overflow, output), ExecutionStatus::IntegerOverflow);
  const auto trace = execution.ExceptionDiagnostic().stack_trace;
  ASSERT_EQ(trace.size(), 2U);
  EXPECT_EQ(trace.front().function, "borrowedFailure");
  EXPECT_EQ(trace.back().function, "calculate");
  EXPECT_EQ(execution.current_frame(), nullptr);
  EXPECT_EQ(isolate.native_control().state().host_execution, nullptr);
  ExpectIntegerOperations(BINDING_ENTRY, execution, {9, 11, 11});
  const std::array stop{Integer(10)};
  ASSERT_EQ(Execute(BINDING_ENTRY, execution, stop, output), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 42);
  EXPECT_FALSE(execution.HasException());
  EXPECT_TRUE(isolate.native_control().IsStopRequested());
  EXPECT_EQ(isolate.native_control().Poll(), ExecutionStatus::ExecutionStopped);
  EXPECT_EQ(execution.current_frame(), nullptr);
  EXPECT_EQ(isolate.native_control().state().host_execution, nullptr);
}

TEST(NativeBindingAbiTest, DirectScalarCallsPreserveBooleanCodecsAndBorrowedRoots) {
  CheckBindings<Step>(0);
}

TEST(NativeBindingAbiTest, SameImageAcceptsCheckedBindingsAndPropagatesTheirErrors) {
  CheckBindings<CheckedStep>(42);
}

}  // namespace binding_test
'''
