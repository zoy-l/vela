"""AOT map locations preserve mutation, nullable values, GC and failed growth."""

SOURCE = r'''
external func freeze(): void = 'lookup.freeze';
func update(map: Map<string, int>, key: string): int {
  const value = (map.get(key) ?? 0) + 1;
  map.set(key, value);
  return value;
}
func touch(map: Map<string, int>): void { map.clear(); map.set('first', 100); }
export func calculate(operation: int): int {
  if (operation == 0) {
    const values = Map<string, int>();
    for (let index = 0; index < 1000; index++) { update(values, (index % 333).toString()); }
    let sum = 0;
    for (let index = 0; index < 333; index++) { sum += values.get(index.toString()) ?? 0; }
    return sum;
  }
  if (operation == 1) {
    const values = Map<string, int?>();
    values.set('nullable', null);
    for (let index = 0; index < 32; index++) {
      const value = (values.get('nullable') ?? 0) + 1;
      values.set('nullable', value);
    }
    return values.get('nullable') ?? 0;
  }
  let map = Map<string, int>();
  map.set('first', 41);
  if (operation == 2) {
    const value = map.get('first') ?? 0;
    touch(map);
    map.set('first', value + 1);
  }
  if (operation == 3) {
    let key = 'first';
    const value = map.get(key) ?? 0;
    key = 'second'; map.set(key, value + 1);
    return map.get('second') ?? 0;
  }
  if (operation == 4) {
    const value = map.get('first') ?? 0;
    map = Map<string, int>(); map.set('first', value + 1);
  }
  if (operation == 5 || operation == 6) {
    let value = 0;
    if (operation == 6) { value = map.get('first') ?? 0; }
    map.set('first', value + 1);
  }
  if (operation == 7) {
    map.set('first', 9223372036854775807);
    try { update(map, 'first'); }
    catch (error: Error) {
      return error.code == 'IntegerOverflow' && map.get('first') == 9223372036854775807 ? 1 : 0;
    }
  }
  if (operation == 8) {
    map.clear();
    for (let index = 0; index < 8; index++) { map.set(index.toString(), index); }
    freeze(); return update(map, 'new');
  }
  return map.get('first') ?? 0;
}
'''

HOST_TESTS = r'''
extern "C" const NativeImage* MAP_LOOKUP_IMAGE() noexcept;

void FreezeMapLookup(NativeContext& context) noexcept {
  auto& heap = context.isolate().heap();
  heap.SetAllocationLimit(heap.allocated_bytes());
}

TEST(NativeMapLookupTest, PreservesGrowthAliasesNullableValuesAndOverflowUnderGc) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<FreezeMapLookup>("lookup.freeze")));
  const auto instance = NativeInstance::Instantiate(isolate, *MAP_LOOKUP_IMAGE());
  ASSERT_TRUE(instance);
  NativeExecution execution(**instance);
  const std::array<std::int64_t, 8> expected{1000, 32, 42, 42, 42, 1, 42, 1};
  for (std::size_t operation = 0; operation < expected.size(); ++operation) {
    const std::array input{Integer(static_cast<std::int64_t>(operation))};
    NativeValue output;
    ASSERT_EQ(Execute(MAP_LOOKUP_ENTRY, execution, input, output), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), expected[operation]) << operation;
    EXPECT_EQ(execution.current_frame(), nullptr);
    EXPECT_EQ(*isolate.heap().native_stack_roots_head(), nullptr);
  }
  const std::array fail{Integer(8)};
  NativeValue output;
  EXPECT_EQ(Execute(MAP_LOOKUP_ENTRY, execution, fail, output), ExecutionStatus::OutOfMemory);
  isolate.heap().SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(execution.current_frame(), nullptr);
  EXPECT_EQ(*isolate.heap().native_stack_roots_head(), nullptr);
  const std::array retry{Integer(0)};
  ASSERT_EQ(Execute(MAP_LOOKUP_ENTRY, execution, retry, output), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(output.payload), 1000);
}
'''
