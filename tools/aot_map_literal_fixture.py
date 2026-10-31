"""Standalone Map literal allocation, insertion and host-boundary contracts."""

SOURCE = """
external func freeze(): int;
let calls = 0;
func key(): int { calls++; return 1; }
func item(): int { calls++; return 42; }
export func calculate(operation: int): int {
  if (operation == 0) { return calls; }
  if (operation == 1) { const map = {key(): item()}; return map.size; }
  const map = {freeze(): item(), key(): item()};
  return map.size;
}
"""

VALUE_SOURCE = """
export func calculate(value: Map<string, int>?): Map<string, int> {
  return value ?? {'answer': 42};
}
"""

HOST_TESTS = r'''
extern "C" const NativeImage* MAP_LITERAL_IMAGE() noexcept;
extern "C" const NativeImage* MAP_VALUE_IMAGE() noexcept;

std::int64_t FreezeMapLiteralHeap(NativeContext& context) noexcept {
  auto& heap = context.isolate().heap();
  heap.SetAllocationLimit(heap.allocated_bytes());
  return 1;
}

TEST(NativeHostTest, MapLiteralAllocationAndInsertionFailuresStopLaterEffects) {
  Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<FreezeMapLiteralHeap>("freeze")));
  const auto instance = NativeInstance::Instantiate(isolate, *MAP_LITERAL_IMAGE());
  ASSERT_TRUE(instance);
  NativeExecution execution(**instance);
  NativeValue result;
  const std::array count{Integer(0)};
  ASSERT_EQ(Execute(MAP_LITERAL_ENTRY, execution, count, result), ExecutionStatus::Returned);
  auto& heap = isolate.heap();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  heap.SetAllocationLimit(baseline);
  const std::array allocate{Integer(1)};
  const auto allocation = Execute(MAP_LITERAL_ENTRY, execution, allocate, result);
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(allocation, ExecutionStatus::OutOfMemory);
  ASSERT_EQ(Execute(MAP_LITERAL_ENTRY, execution, count, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 0);
  const std::array insert{Integer(2)};
  const auto insertion = Execute(MAP_LITERAL_ENTRY, execution, insert, result);
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(insertion, ExecutionStatus::OutOfMemory);
  ASSERT_EQ(Execute(MAP_LITERAL_ENTRY, execution, count, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 1);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), baseline);
}

TEST(NativeHostTest, MapLiteralResultsKeepTheirConcreteTypeAcrossThePublicAbi) {
  Isolate isolate(1);
  auto instance = MAP_VALUE_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(instance);
  const auto* image = MAP_VALUE_IMAGE();
  const auto index = PublicCalculateIndex(*image);
  VelaAotValue argument{};
  VelaAotValue output{};
  ASSERT_EQ(vela_aot_v1_invoke(instance->get(), image, index, &argument, 1, &output, nullptr),
            VELA_AOT_RETURNED);
  auto map = OwnPublicValue(output);
  ASSERT_NE(map, nullptr);
  isolate.heap().Collect();
  VelaAotValue repeated{};
  ASSERT_EQ(vela_aot_v1_invoke(instance->get(), image, index, &output, 1, &repeated, nullptr),
            VELA_AOT_RETURNED);
  auto same = OwnPublicValue(repeated);
  ASSERT_NE(same, nullptr);
  const char* text = "wrong";
  VelaAotHandle* raw = nullptr;
  ASSERT_EQ(vela_aot_v1_string_create(&isolate, text, 5, &raw), VELA_AOT_RETURNED);
  auto wrong = OwnPublicHandle(raw);
  argument = {.payload = reinterpret_cast<std::uintptr_t>(wrong.get()), .kind = VELA_AOT_HANDLE};
  EXPECT_EQ(vela_aot_v1_invoke(instance->get(), image, index, &argument, 1, &repeated, nullptr),
            static_cast<VelaAotStatus>(ExecutionStatus::InvalidCast));
}
'''
