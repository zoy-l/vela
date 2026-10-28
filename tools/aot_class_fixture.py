"""Generated and explicit class constructors preserve initialization contracts in AOT."""

SOURCE = """
external func collect(): void;
class Trace {
  static value: int = 0;
  static mark(value: int): int {
    Trace.value = Trace.value * 10 + value;
    collect();
    return value;
  }
}
class Record {
  value: int;
  text: string?;
  step: int = Trace.mark(8);
  values: int[] = [];
  private hidden: int = Trace.mark(3);
  constructor { Trace.mark(4); }
}
class Explicit<T> {
  value: T;
  text: string = 'default';
  private delta: int;
  constructor(initial: T) { this.value = initial; this.delta = 2; }
  adjustment(): int { return this.delta; }
}
class Base { value: int; constructor { this.value++; } }
class Child extends Base {
  extra: int;
  constructor { super(value: this.extra * 20 - 1); }
}
class Optional { text: string? = null; }
export func calculate(operation: int): int {
  Trace.value = 0;
  if (operation == 0) {
    const record = Record(step: Trace.mark(2), value: Trace.mark(1), text: null);
    return Trace.value == 2134 && record.value == 1 && record.step == 2 &&
           record.text == null && record.values.length == 0 ? 42 : 0;
  }
  if (operation == 1) {
    const first = Record(value: 40, text: null);
    const second = Record(value: 40, text: 'retained');
    first.values.push(42);
    collect();
    return Trace.value == 834834 && first.values[0] == 42 && second.values.length == 0 &&
           second.text == 'retained' && Optional().text == null ? 42 : 0;
  }
  if (operation == 2) {
    const box = Explicit<int>(initial: 40);
    collect();
    return box.text == 'default' ? box.value + box.adjustment() : 0;
  }
  if (operation == 3) {
    const child = Child(extra: 2);
    collect();
    return child.value + child.extra;
  }
  const box = Explicit<string>(initial: 'retained');
  collect();
  return box.value == 'retained' && box.adjustment() == 2 ? 42 : 0;
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* CLASS_IMAGE() noexcept;

TEST(NativeClassTest, ConstructorsPreserveArgumentsDefaultsPrivateStateGenericsAndSuperUnderGc) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  const auto instance = NativeInstance::Instantiate(isolate, *CLASS_IMAGE());
  ASSERT_TRUE(instance.has_value()) << instance.error().message;
  NativeExecution execution(**instance);
  for (const auto operation : {0, 1, 2, 3, 4}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(CLASS_ENTRY, execution, arguments, result), ExecutionStatus::Returned)
        << operation;
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}
'''
