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
  FreezeAllocations(context);
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
  UnfreezeAllocations(isolate);
  EXPECT_EQ(allocation, ExecutionStatus::OutOfMemory);
  ASSERT_EQ(Execute(MAP_LITERAL_ENTRY, execution, count, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 0);
  const std::array insert{Integer(2)};
  const auto insertion = Execute(MAP_LITERAL_ENTRY, execution, insert, result);
  UnfreezeAllocations(isolate);
  EXPECT_EQ(insertion, ExecutionStatus::OutOfMemory);
  ASSERT_EQ(Execute(MAP_LITERAL_ENTRY, execution, count, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 1);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), baseline);
}

TEST(NativeHostTest, MapLiteralResultsKeepTheirConcreteTypeAcrossThePublicAbi) {
  Isolate isolate(1);
  vela::HandleScope scope(isolate);
  auto instance = MAP_VALUE_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(instance);
  const auto* image = MAP_VALUE_IMAGE();
  const auto index = PublicCalculateIndex(*image);
  VelaAotValue argument{};
  VelaAotValue output{};
  ASSERT_EQ(vela_aot_v2_invoke(&scope, instance->get(), image, index, &argument, 1, &output, nullptr),
            VELA_AOT_RETURNED);
  ASSERT_EQ(output.kind, VELA_AOT_LOCAL);
  isolate.heap().Collect();
  VelaAotValue repeated{};
  ASSERT_EQ(vela_aot_v2_invoke(&scope, instance->get(), image, index, &output, 1, &repeated, nullptr),
            VELA_AOT_RETURNED);
  ASSERT_EQ(repeated.kind, VELA_AOT_LOCAL);
  const char* text = "wrong";
  VelaAotLocal wrong{};
  ASSERT_EQ(vela_aot_v2_string_create(&scope, text, 5, &wrong), VELA_AOT_RETURNED);
  argument = {.kind = VELA_AOT_LOCAL, .local = wrong};
  EXPECT_EQ(vela_aot_v2_invoke(&scope, instance->get(), image, index, &argument, 1, &repeated, nullptr),
            static_cast<VelaAotStatus>(ExecutionStatus::InvalidCast));
}
'''
