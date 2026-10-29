"""Exercise direct and checked host ABIs against the same emitted AOT image."""

SOURCE = r'''
external func step(value: int): int = 'binding.step';
external func invert(value: bool): bool = 'binding.invert';
external func measure(text: string): int = 'binding.measure';
external func combine(value: int, scale: double, enabled: bool): double = 'binding.combine';
external func collect(): void = 'binding.collect';
external func scale(value: double): double = 'binding.scale';
external func noop(): void = 'binding.noop';

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
  try { return step(-1); }
  catch (error: Error) {
    if (error.code == 'NativeFailure' && error.message == 'negative input') { return 42; }
    return -1;
  }
}
'''

HOST_TESTS = r'''
extern "C" const NativeImage* BINDING_IMAGE() noexcept;
namespace binding_test {

std::int64_t Step(std::int64_t value) noexcept { return value + 1; }
std::expected<std::int64_t, NativeError> CheckedStep(std::int64_t value) noexcept {
  if (value < 0) { return std::unexpected(NativeError{.message = "negative input"}); }
  return value + 1;
}
bool Invert(bool value) noexcept { return !value; }
double Scale(double value) noexcept { return value * 2.0; }
void Noop() noexcept {}
double Combine(NativeContext&, std::int64_t value, double scale, bool enabled) noexcept {
  return enabled ? static_cast<double>(value) * scale : 0.0;
}
std::int64_t Measure(NativeContext& context, std::string_view text) noexcept {
  context.isolate().heap().Collect();
  return static_cast<std::int64_t>(text.size());
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
  const auto instance = NativeInstance::Instantiate(isolate, *BINDING_IMAGE());
  ASSERT_TRUE(instance.has_value()) << instance.error().message;
  NativeExecution execution(**instance);
  const std::array<std::int64_t, 4> expected{500500, 42, 16, failure_result};
  for (std::size_t operation = 0; operation < expected.size(); ++operation) {
    const std::array arguments{Integer(static_cast<std::int64_t>(operation))};
    NativeValue result;
    ASSERT_EQ(Execute(BINDING_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), expected.at(operation)) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}

TEST(NativeBindingAbiTest, DirectScalarCallsPreserveBooleanCodecsAndBorrowedRoots) {
  CheckBindings<Step>(0);
}

TEST(NativeBindingAbiTest, SameImageAcceptsCheckedBindingsAndPropagatesTheirErrors) {
  CheckBindings<CheckedStep>(42);
}

}  // namespace binding_test
'''
