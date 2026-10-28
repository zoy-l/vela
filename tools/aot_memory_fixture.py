"""Typed native field and array storage contracts under collection and mutation."""

SOURCE = """
external func collect(): void;
enum Choice { first, second }
class Record {
  number: int = 0;
  decimal: double = 0.0;
  enabled: bool = false;
  maybe: int? = null;
  maybeDouble: double? = null;
  maybeBool: bool? = null;
  choice: Choice? = null;
  text: string? = null;
  any: dynamic = null;
}
let effect = 0;
func next(): int { effect += 1; return effect; }
func grow(values: int[]): void {
  for (let index = 0; index < 100; index++) { values.push(index); }
}
export func calculate(operation: int): int {
  if (operation == 0) {
    const record = Record();
    record.number = -9223372036854775807 - 1;
    record.decimal = -0.0;
    record.enabled = true;
    record.maybe = 9223372036854775807;
    record.maybeDouble = 1.5;
    record.maybeBool = false;
    record.choice = Choice.second;
    record.text = 'retained:' + operation.toString();
    record.any = 'dynamic';
    collect();
    const valid = record.number == -9223372036854775807 - 1 && record.decimal == 0.0 &&
      record.enabled && record.maybe == 9223372036854775807 && record.maybeDouble == 1.5 &&
      record.maybeBool == false && record.choice == Choice.second &&
      record.text == 'retained:0' && (record.any as string) == 'dynamic';
    record.maybe = null; record.maybeDouble = null; record.maybeBool = null;
    record.choice = null; record.text = null; record.any = null;
    return valid && record.maybe == null && record.maybeDouble == null &&
      record.maybeBool == null && record.choice == null && record.text == null &&
      record.any == null ? 42 : 0;
  }
  if (operation == 1) {
    const ints: int?[] = [null, 1];
    const doubles: double?[] = [null, 1.5];
    const bools: bool?[] = [null, false];
    const choices: Choice?[] = [null, Choice.first];
    ints[0] = 9223372036854775807; doubles[0] = -2.5;
    bools[0] = true; choices[0] = Choice.second;
    ints.push(null); doubles.push(null); bools.push(null); choices.push(null);
    collect();
    const valid = ints[0] == 9223372036854775807 && doubles[0] == -2.5 && bools[0] == true &&
      choices[0] == Choice.second && ints[1] == 1 && doubles[1] == 1.5 && bools[1] == false &&
      choices[1] == Choice.first && ints[2] == null && doubles[2] == null &&
      bools[2] == null && choices[2] == null;
    ints[0] = null; doubles[0] = null; bools[0] = null; choices[0] = null;
    return valid && ints[0] == null && doubles[0] == null && bools[0] == null &&
      choices[0] == null ? 42 : 0;
  }
  if (operation == 2) {
    const texts: string?[] = [null];
    const records: Record?[] = [null];
    const values: dynamic[] = [null];
    for (let index = 0; index < 100; index++) {
      const text = 'item:' + index.toString();
      const record = Record(number: index, text: text);
      texts.push(text); records.push(record); values.push(record);
      collect();
    }
    const record = records[100];
    if (record == null) { return 0; }
    const valid = texts[0] == null && records[0] == null && values[0] == null &&
      texts[100] == 'item:99' && record.number == 99 && record.text == 'item:99' &&
      (values[100] as Record).number == 99;
    texts[100] = null; records[100] = null; values[100] = null;
    return valid && texts[100] == null && records[100] == null && values[100] == null ? 42 : 0;
  }
  if (operation == 3) {
    const bools = [false, true]; const doubles = [-0.0, 2.5];
    bools[0] = true; doubles[0] = -1.5;
    bools.push(false); doubles.push(4.5);
    const values: int[] = [7]; const alias = values;
    const before = values[0]; grow(alias);
    return before == 7 && values[100] == 99 && values.length == 101 &&
      bools[0] && bools[1] && !bools[2] && doubles[0] == -1.5 && doubles[2] == 4.5 ? 42 : 0;
  }
  if (operation == 4) {
    const values: int[] = [];
    effect = 0;
    values.push(next()); values.push(next(), next()); values.unshift(0);
    return effect == 3 && values.length == 4 && values[0] == 0 && values[1] == 1 &&
      values[2] == 2 && values[3] == 3 ? 42 : 0;
  }
  if (operation == 8) {
    const erased: dynamic = [1, 2, 3] as dynamic;
    const values = erased as int[];
    values.push(4);
    const first: dynamic = values[0] as dynamic;
    return values.length == 4 && (first as int) == 1 && values[3] == 4 ? 42 : 0;
  }
  const values = ['rooted:' + operation.toString()];
  const index = operation == 5 ? -1 : operation == 6 ? 4294967296 : -4294967296;
  try { return values[index].length; }
  catch (error: Error) {
    return error.code == 'IndexOutOfBounds' && values[0] == 'rooted:' + operation.toString() ? 42 : 0;
  }
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* MEMORY_IMAGE() noexcept;

TEST(NativeMemoryTest, FieldsAndArraysPreserveTypedNullableAndDynamicStorage) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  const auto instance = NativeInstance::Instantiate(isolate, *MEMORY_IMAGE());
  ASSERT_TRUE(instance.has_value()) << instance.error().message;
  NativeExecution execution(**instance);
  for (const auto operation : {0, 1, 2, 3, 4, 8}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(MEMORY_ENTRY, execution, arguments, result), ExecutionStatus::Returned)
        << operation;
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}

TEST(NativeMemoryTest, WideAndNegativeIndicesKeepExceptionRootsAndCleanupFrames) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  const auto instance = NativeInstance::Instantiate(isolate, *MEMORY_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  for (const auto operation : {5, 6, 7}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(MEMORY_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}
'''
