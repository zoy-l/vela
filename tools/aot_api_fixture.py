"""Common local scopes, persistent roots and typed AOT host contracts."""

SOURCE = """
external func collect(): void;
export class Box { text: string = 'retained'; }
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
#include <vela/host.h>
#include "runtime/native/native_api.h"
extern "C" const NativeImage* PUBLIC_IMAGE() noexcept;

using PublicPersistent = std::unique_ptr<VelaAotPersistent, decltype(&vela_aot_v2_persistent_release)>;
using PublicInstance = std::unique_ptr<VelaAotInstance, decltype(&vela_aot_v2_instance_release)>;
using PublicScope = std::unique_ptr<VelaAotScope, decltype(&vela_aot_v2_scope_release)>;
using PublicError = std::unique_ptr<VelaAotError, decltype(&vela_aot_v2_error_release)>;
std::uint32_t PublicCalculateIndex(const NativeImage& image) {
  const auto functions = image.functions.values();
  for (std::size_t index = 0; index < functions.size(); ++index) {
    if (functions[index].name.text() == "calculate") { return static_cast<std::uint32_t>(index); }
  }
  return kNoNativeIndex;
}

TEST(NativeHostTest, PublicManagedResultsRetainObjectsAfterInstancesAndScopesRelease) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  const auto baseline = isolate.heap().object_count();
  PublicPersistent retained(nullptr, vela_aot_v2_persistent_release);
  {
    VelaAotScope* raw_scope = nullptr;
    ASSERT_EQ(vela_aot_v2_scope_create(&isolate, &raw_scope), VELA_AOT_RETURNED);
    PublicScope scope(raw_scope, vela_aot_v2_scope_release);
    VelaAotInstance* raw = nullptr;
    ASSERT_EQ(vela_aot_v2_instantiate(&isolate, PUBLIC_IMAGE(), &raw, nullptr), VELA_AOT_RETURNED);
    PublicInstance instance(raw, vela_aot_v2_instance_release);
    VelaAotValue empty{}, output{};
    ASSERT_EQ(vela_aot_v2_invoke(scope.get(), instance.get(), PUBLIC_IMAGE(),
                                 PublicCalculateIndex(*PUBLIC_IMAGE()), &empty, 1, &output, nullptr),
              VELA_AOT_RETURNED);
    ASSERT_EQ(output.kind, VELA_AOT_LOCAL);
    VelaAotPersistent* root = nullptr;
    ASSERT_EQ(vela_aot_v2_persist(output.local, &root), VELA_AOT_RETURNED);
    retained.reset(root);
  }
  isolate.heap().Collect();
  EXPECT_GT(isolate.heap().object_count(), baseline);
  {
    vela::HandleScope scope(isolate);
    VelaAotLocal local{};
    ASSERT_EQ(vela_aot_v2_localize(&scope, retained.get(), &local), VELA_AOT_RETURNED);
    retained.reset();
    VelaAotInstance* raw = nullptr;
    ASSERT_EQ(vela_aot_v2_instantiate(&isolate, PUBLIC_IMAGE(), &raw, nullptr), VELA_AOT_RETURNED);
    PublicInstance instance(raw, vela_aot_v2_instance_release);
    VelaAotValue argument{.kind = VELA_AOT_LOCAL, .local = local}, output{};
    ASSERT_EQ(vela_aot_v2_invoke(&scope, instance.get(), PUBLIC_IMAGE(),
                                 PublicCalculateIndex(*PUBLIC_IMAGE()), &argument, 1, &output, nullptr),
              VELA_AOT_RETURNED);
    const auto original = vela::runtime::aot::Decode(scope, argument);
    const auto copied = vela::runtime::aot::Decode(scope, output);
    ASSERT_TRUE(original);
    ASSERT_TRUE(copied);
    EXPECT_EQ(original->AsObject(), copied->AsObject());
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

TEST(NativeHostTest, PublicBoundaryChecksIsolateNominalTypesCanonicalValuesAndUtf8) {
  Isolate isolate(1), foreign(1);
  vela::HandleScope scope(isolate), other(foreign);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  auto instance = PUBLIC_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(instance);
  VelaAotLocal text{};
  ASSERT_EQ(vela_aot_v2_string_create(&other, "foreign", 7, &text), VELA_AOT_RETURNED);
  VelaAotValue argument{.kind = VELA_AOT_LOCAL, .local = text}, output{};
  const auto invoke = [&] { return vela_aot_v2_invoke(&scope, instance->get(), PUBLIC_IMAGE(),
      PublicCalculateIndex(*PUBLIC_IMAGE()), &argument, 1, &output, nullptr); };
  EXPECT_EQ(invoke(), static_cast<VelaAotStatus>(ExecutionStatus::InvalidIsolate));
  ASSERT_EQ(vela_aot_v2_string_create(&scope, "local", 5, &text), VELA_AOT_RETURNED);
  argument.local = text;
  EXPECT_EQ(invoke(), static_cast<VelaAotStatus>(ExecutionStatus::InvalidCast));
  argument = {.payload = 2, .kind = VELA_AOT_BOOLEAN};
  EXPECT_EQ(invoke(), static_cast<VelaAotStatus>(ExecutionStatus::TypeError));
  const auto invalid = std::string(1, static_cast<char>(0xff));
  EXPECT_EQ(vela_aot_v2_string_create(&scope, invalid.data(), 1, &text),
            static_cast<VelaAotStatus>(ExecutionStatus::NativeFailure));
  const char* borrowed = nullptr;
  std::uint64_t size = 0;
  ASSERT_EQ(vela_aot_v2_string_view(text, &borrowed, &size), VELA_AOT_RETURNED);
  EXPECT_EQ(std::string_view(borrowed, size), "local");
}

TEST(NativeHostTest, PublicFutureAndErrorsTransferRootedPayloadsAndLogicalTraces) {
  Isolate isolate(1);
  vela::HandleScope scope(isolate);
  RegisterComposition(isolate);
  auto instance = COMPOSITION_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(instance);
  VelaAotValue argument{.payload = 1, .kind = VELA_AOT_INTEGER}, output{};
  ASSERT_EQ(vela_aot_v2_invoke(&scope, instance->get(), COMPOSITION_IMAGE(),
      PublicCalculateIndex(*COMPOSITION_IMAGE()), &argument, 1, &output, nullptr), VELA_AOT_RETURNED);
  instance->reset();
  isolate.heap().Collect();
  VelaAotValue completed{};
  ASSERT_EQ(vela_aot_v2_future_wait(&scope, output.local, 0, &completed, nullptr), VELA_AOT_RETURNED);
  EXPECT_EQ(completed.kind, VELA_AOT_INTEGER);
  EXPECT_EQ(std::bit_cast<std::int64_t>(completed.payload), 42);
  auto integers = INTEGER_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(integers);
  const std::array arguments{VelaAotValue{.payload = 1, .kind = VELA_AOT_INTEGER},
      VelaAotValue{.payload = 1, .kind = VELA_AOT_INTEGER}, VelaAotValue{.kind = VELA_AOT_INTEGER}};
  VelaAotError* failure = nullptr;
  const auto status = vela_aot_v2_invoke(&scope, integers->get(), INTEGER_IMAGE(),
      PublicCalculateIndex(*INTEGER_IMAGE()), arguments.data(), arguments.size(), &output, &failure);
  PublicError error(failure, vela_aot_v2_error_release);
  EXPECT_STREQ(vela_aot_v2_status_code(status), "DivisionByZero");
  integers->reset();
  isolate.heap().Collect();
  const char* message = nullptr;
  std::uint64_t size = 0;
  ASSERT_EQ(vela_aot_v2_error_message(error.get(), &message, &size), VELA_AOT_RETURNED);
  EXPECT_EQ(std::string_view(message, size), "division by zero");
  VelaAotTraceFrame frame{};
  ASSERT_EQ(vela_aot_v2_error_frame(error.get(), 0, &frame), VELA_AOT_RETURNED);
  EXPECT_GT(frame.line, 0U);
  EXPECT_EQ(std::string_view(frame.function, frame.function_size), "calculate");
}

TEST(NativeHostTest, GeneratedCppExportsKeepCompleteTypesAndUseTheCallersScope) {
  Isolate isolate(1);
  vela::HandleScope scope(isolate);
  auto integers = INTEGER_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(integers);
  const auto product = INTEGER_NAMESPACE::Call_calculate(scope, integers->get(), 0, 6, 7);
  ASSERT_TRUE(product);
  EXPECT_EQ(*product, 42);
  const auto failed = INTEGER_NAMESPACE::Call_calculate(scope, integers->get(), 1, 1, 0);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().status, ExecutionStatus::DivisionByZero);
  EXPECT_EQ(failed.error().message, "division by zero");
  auto booleans = BOOL_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(booleans);
  const auto negated = BOOL_NAMESPACE::Call_calculate(scope, booleans->get(), true);
  ASSERT_TRUE(negated);
  EXPECT_FALSE(*negated);
  auto nullable = NULLABLE_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(nullable);
  const auto present = NULLABLE_NAMESPACE::Call_calculate(scope, nullable->get(), std::optional<std::int64_t>(42));
  ASSERT_TRUE(present);
  ASSERT_TRUE(*present);
  EXPECT_EQ(**present, 42);
  const auto absent = NULLABLE_NAMESPACE::Call_calculate(scope, nullable->get(), std::nullopt);
  ASSERT_TRUE(absent);
  EXPECT_FALSE(*absent);
  auto wrong_image = NULL_TEST_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(wrong_image);
  const auto wrong = NULLABLE_NAMESPACE::Call_calculate(scope, wrong_image->get(), std::optional<std::int64_t>(42));
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error().status, ExecutionStatus::InvalidEntry);
  auto floats = FLOAT_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(floats);
  const auto divided = FLOAT_NAMESPACE::Call_calculate(scope, floats->get(), 84.0, 2.0);
  ASSERT_TRUE(divided);
  EXPECT_EQ(*divided, 42.0);
  auto empty = VOID_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(empty);
  ASSERT_TRUE(VOID_NAMESPACE::Call_calculate(scope, empty->get()));
  RegisterComposition(isolate);
  auto objects = PUBLIC_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(objects);
  auto object = PUBLIC_NAMESPACE::Call_calculate(scope, objects->get(), std::nullopt);
  ASSERT_TRUE(object);
  const auto type = vela::Type<PUBLIC_NAMESPACE::Type_Box>::Bind(**objects);
  ASSERT_TRUE(type);
  objects->reset();
  isolate.heap().Collect();
  const auto text = PUBLIC_NAMESPACE::Type_BoxMembers::Get_text(*type, scope, *object);
  ASSERT_TRUE(text);
  EXPECT_EQ(text->Utf8(), "retained");
  auto async = COMPOSITION_NAMESPACE::Instantiate(&isolate);
  ASSERT_TRUE(async);
  auto future = COMPOSITION_NAMESPACE::Call_calculate(scope, async->get(), 1);
  ASSERT_TRUE(future);
  async->reset();
  isolate.heap().Collect();
  auto result = future->Wait(scope);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 42);
}
'''
