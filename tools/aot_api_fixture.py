"""Owned host ABI contract fixtures for statically linked native images."""

SOURCE = """
external func collect(): void;
class Box { public param text: string = 'retained'; }
export func calculate(value: Box?): Box {
  collect();
  if (value != null) {
    if (value.text != 'retained') { throw Error(message: 'lost object'); }
    return value;
  }
  return Box();
}
"""

HOST_TESTS = r'''
#include "runtime/native_api.h"
extern "C" const NativeImage* PUBLIC_IMAGE() noexcept;

using PublicHandle = std::unique_ptr<VelaAotHandle, decltype(&vela_aot_v1_handle_release)>;
using PublicInstance = std::unique_ptr<VelaAotInstance, decltype(&vela_aot_v1_instance_release)>;
PublicHandle OwnPublicHandle(VelaAotHandle* handle) { return {handle, vela_aot_v1_handle_release}; }
PublicHandle OwnPublicValue(VelaAotValue value) {
  return OwnPublicHandle(value.kind == VELA_AOT_HANDLE
                             ? reinterpret_cast<VelaAotHandle*>(value.payload) : nullptr);
}
std::uint32_t PublicCalculateIndex(const NativeImage& image) {
  const auto functions = image.functions.values();
  for (std::size_t index = 0; index < functions.size(); ++index) {
    if (functions.subspan(index).front().name.text() == "calculate") {
      return static_cast<std::uint32_t>(index);
    }
  }
  return kNoNativeIndex;
}

TEST(NativeHostTest, PublicManagedResultsRetainObjectsAfterInstancesAndInputsRelease) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  const auto baseline = isolate.heap().object_count();
  {
    VelaAotInstance* raw = nullptr;
    ASSERT_EQ(vela_aot_v1_instantiate(&isolate, PUBLIC_IMAGE(), &raw, nullptr),
              VELA_AOT_RETURNED);
    PublicInstance first(raw, vela_aot_v1_instance_release);
    const auto index = PublicCalculateIndex(*PUBLIC_IMAGE());
    const VelaAotValue empty{};
    VelaAotValue output{};
    ASSERT_EQ(vela_aot_v1_invoke(first.get(), PUBLIC_IMAGE(), index, &empty, 1, &output, nullptr), VELA_AOT_RETURNED);
    ASSERT_EQ(output.kind, VELA_AOT_HANDLE);
    auto original = OwnPublicValue(output);
    first.reset();
    isolate.heap().Collect();
    raw = nullptr;
    ASSERT_EQ(vela_aot_v1_instantiate(&isolate, PUBLIC_IMAGE(), &raw, nullptr),
              VELA_AOT_RETURNED);
    PublicInstance second(raw, vela_aot_v1_instance_release);
    VelaAotValue copied{};
    ASSERT_EQ(vela_aot_v1_invoke(second.get(), PUBLIC_IMAGE(), index, &output, 1, &copied, nullptr), VELA_AOT_RETURNED);
    auto retained = OwnPublicValue(copied);
    original.reset();
    second.reset();
    isolate.heap().Collect();
    EXPECT_GT(isolate.heap().object_count(), baseline);
    retained.reset();
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

TEST(NativeHostTest, PublicBoundaryChecksIsolateNominalTypesCanonicalValuesAndUtf8) {
  Isolate isolate(1);
  Isolate foreign(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  VelaAotInstance* raw = nullptr;
  ASSERT_EQ(vela_aot_v1_instantiate(&isolate, PUBLIC_IMAGE(), &raw, nullptr),
            VELA_AOT_RETURNED);
  PublicInstance instance(raw, vela_aot_v1_instance_release);
  const auto index = PublicCalculateIndex(*PUBLIC_IMAGE());
  VelaAotHandle* text = nullptr;
  ASSERT_EQ(vela_aot_v1_string_create(&foreign, "foreign", 7, &text),
            VELA_AOT_RETURNED);
  auto foreign_text = OwnPublicHandle(text);
  VelaAotValue argument{.payload = reinterpret_cast<std::uintptr_t>(text), .kind = VELA_AOT_HANDLE};
  VelaAotValue output{};
  EXPECT_EQ(vela_aot_v1_invoke(instance.get(), PUBLIC_IMAGE(), index, &argument, 1, &output, nullptr),
            static_cast<VelaAotStatus>(ExecutionStatus::InvalidIsolate));
  text = nullptr;
  ASSERT_EQ(vela_aot_v1_string_create(&isolate, "local", 5, &text),
            VELA_AOT_RETURNED);
  auto local_text = OwnPublicHandle(text);
  argument.payload = reinterpret_cast<std::uintptr_t>(text);
  EXPECT_EQ(vela_aot_v1_invoke(instance.get(), PUBLIC_IMAGE(), index, &argument, 1, &output, nullptr),
            static_cast<VelaAotStatus>(ExecutionStatus::InvalidCast));
  argument = {.payload = 2, .kind = VELA_AOT_BOOLEAN};
  EXPECT_EQ(vela_aot_v1_invoke(instance.get(), PUBLIC_IMAGE(), index, &argument, 1, &output, nullptr),
            static_cast<VelaAotStatus>(ExecutionStatus::TypeError));
  const auto invalid_utf8 = std::string(1, static_cast<char>(0xff));
  VelaAotHandle* invalid = nullptr;
  EXPECT_EQ(vela_aot_v1_string_create(&isolate, invalid_utf8.data(), 1, &invalid),
            static_cast<VelaAotStatus>(ExecutionStatus::TypeError));
  EXPECT_EQ(invalid, nullptr);
  const char* borrowed = nullptr;
  std::uint64_t size = 0;
  ASSERT_EQ(vela_aot_v1_string_view(local_text.get(), &borrowed, &size), VELA_AOT_RETURNED);
  EXPECT_EQ(std::string_view(borrowed, size), "local");
}

TEST(NativeHostTest, PublicFutureAndErrorsTransferOwnedValuesAndLogicalTraces) {
  Isolate isolate(1);
  RegisterComposition(isolate);
  VelaAotInstance* raw = nullptr;
  ASSERT_EQ(vela_aot_v1_instantiate(&isolate, COMPOSITION_IMAGE(), &raw, nullptr),
            VELA_AOT_RETURNED);
  PublicInstance instance(raw, vela_aot_v1_instance_release);
  VelaAotValue argument{.payload = 1, .kind = VELA_AOT_INTEGER};
  VelaAotValue output{};
  ASSERT_EQ(vela_aot_v1_invoke(instance.get(), COMPOSITION_IMAGE(), PublicCalculateIndex(*COMPOSITION_IMAGE()),
                               &argument, 1, &output, nullptr), VELA_AOT_RETURNED);
  auto future = OwnPublicValue(output);
  ASSERT_NE(future, nullptr);
  instance.reset();
  isolate.heap().Collect();
  VelaAotValue completed{};
  ASSERT_EQ(vela_aot_v1_future_wait(future.get(), 0, &completed, nullptr), VELA_AOT_RETURNED);
  EXPECT_EQ(completed.kind, VELA_AOT_INTEGER);
  EXPECT_EQ(std::bit_cast<std::int64_t>(completed.payload), 42);
  raw = nullptr;
  ASSERT_EQ(vela_aot_v1_instantiate(&isolate, INTEGER_IMAGE(), &raw, nullptr),
            VELA_AOT_RETURNED);
  instance.reset(raw);
  const std::array arguments{
      VelaAotValue{.payload = 1, .kind = VELA_AOT_INTEGER},
      VelaAotValue{.payload = 1, .kind = VELA_AOT_INTEGER}, VelaAotValue{.kind = VELA_AOT_INTEGER}};
  VelaAotHandle* failure = nullptr;
  const auto status = vela_aot_v1_invoke(instance.get(), INTEGER_IMAGE(), PublicCalculateIndex(*INTEGER_IMAGE()),
                                         arguments.data(), arguments.size(), &output, &failure);
  auto error = OwnPublicHandle(failure);
  EXPECT_STREQ(vela_aot_v1_status_code(status), "DivisionByZero");
  instance.reset();
  isolate.heap().Collect();
  const char* message = nullptr;
  std::uint64_t size = 0;
  ASSERT_EQ(vela_aot_v1_error_message(error.get(), &message, &size), VELA_AOT_RETURNED);
  EXPECT_EQ(std::string_view(message, size), "division by zero");
  VelaAotTraceFrame frame{};
  ASSERT_EQ(vela_aot_v1_error_frame(error.get(), 0, &frame), VELA_AOT_RETURNED);
  EXPECT_GT(frame.line, 0U);
  EXPECT_EQ(std::string_view(frame.function, frame.function_size), "calculate");
}

TEST(NativeHostTest, GeneratedCAndCppExportsUseTypedArgumentsResultsAndOwnedHandles) {
  Isolate isolate(1);
  auto integers = INTEGER_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(integers.has_value());
  std::int64_t product = 0;
  ASSERT_EQ(INTEGER_WRAPPER(integers->get(), 0, 6, 7, &product, nullptr), VELA_AOT_RETURNED);
  EXPECT_EQ(product, 42);
  const auto failed = INTEGER_NAMESPACE::Call_calculate(integers->get(), 1, 1, 0);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code(), "DivisionByZero");
  EXPECT_EQ(failed.error().message(), "division by zero");
  auto booleans = BOOL_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(booleans.has_value());
  const auto negated = BOOL_NAMESPACE::Call_calculate(booleans->get(), true);
  ASSERT_TRUE(negated.has_value());
  EXPECT_FALSE(*negated);
  auto nullable = NULLABLE_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(nullable.has_value());
  const auto present = NULLABLE_NAMESPACE::Call_calculate(nullable->get(), std::optional<std::int64_t>(42));
  ASSERT_TRUE(present.has_value());
  ASSERT_TRUE(present->has_value());
  EXPECT_EQ(**present, 42);
  const auto absent = NULLABLE_NAMESPACE::Call_calculate(nullable->get(), std::nullopt);
  ASSERT_TRUE(absent.has_value());
  EXPECT_FALSE(absent->has_value());
  auto null_test = NULL_TEST_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(null_test.has_value());
  const auto wrong_image = NULLABLE_NAMESPACE::Call_calculate(null_test->get(), std::optional<std::int64_t>(42));
  ASSERT_FALSE(wrong_image.has_value());
  EXPECT_EQ(wrong_image.error().code(), "InvalidEntry");
  auto floats = FLOAT_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(floats.has_value());
  const auto divided = FLOAT_NAMESPACE::Call_calculate(floats->get(), 84.0, 2.0);
  ASSERT_TRUE(divided.has_value());
  EXPECT_EQ(*divided, 42.0);
  auto empty = VOID_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(empty.has_value());
  ASSERT_TRUE(VOID_NAMESPACE::Call_calculate(empty->get()).has_value());
  RegisterComposition(isolate);
  auto objects = PUBLIC_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(objects.has_value());
  const vela::runtime::aot::Handle<vela::runtime::aot::AnyTag> nothing;
  auto object = PUBLIC_NAMESPACE::Call_calculate(objects->get(), nothing);
  ASSERT_TRUE(object.has_value());
  objects->reset();
  isolate.heap().Collect();
  EXPECT_NE(object->get(), nullptr);
  auto async = COMPOSITION_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(async.has_value());
  auto future = COMPOSITION_NAMESPACE::Call_calculate(async->get(), 1);
  ASSERT_TRUE(future.has_value());
  async->reset();
  isolate.heap().Collect();
  auto result = vela::runtime::aot::Wait(*future);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->raw().kind, VELA_AOT_INTEGER);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result->raw().payload), 42);
}
'''
