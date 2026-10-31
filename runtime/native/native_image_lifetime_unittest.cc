#include "runtime/native/native_instance.h"

#include <array>
#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/layout_description.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_async.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_image_test_support.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

using test::Table;
using test::Text;

// 动态代码所有者同时提供映像及其全部借用表；别名 shared_ptr 不能只保活映像头。
struct AsyncImage final {
  AsyncImage() {
    image.abi_identity = Text(NativeRuntimeAbiIdentity());
    image.target = Text(NativeTargetIdentity());
    types.front().kind = static_cast<std::uint32_t>(layout::Type::Kind::Future);
    signatures.front().name = Text("Future<int>");
    signatures.front().result.kind = static_cast<std::uint32_t>(layout::Type::Kind::Integer);
    signatures.front().has_result = 1;
    functions.front().name = Text("previousAsync");
    functions.front().result_type = 0;
    functions.front().flags = kNativeAsyncFunction;
    strings.front() = Text("old");
    image.types = Table(types);
    image.signatures = Table(signatures);
    image.functions = Table(functions);
    image.strings = Table(strings);
  }

  NativeImage image;
  std::array<NativeTypeDescription, 1> types;
  std::array<NativeSignatureDescription, 1> signatures;
  std::array<NativeFunctionDescription, 1> functions;
  std::array<NativeText, 1> strings;
};

std::uint8_t ResumePreviousVersion(NativeExecution* execution, NativeResumeState* state) noexcept {
  NativeValue completion;
  const auto status = vela_native_v1_resume_outcome(execution, state, &completion);
  if (static_cast<ExecutionStatus>(status) != ExecutionStatus::Returned) {
    return status;
  }
  const auto constant = execution->instance().constant(0).AsString()->value();
  const auto value =
      DecodeNativeValue(completion).AsInteger() + static_cast<std::int64_t>(constant.size());
  const auto result = EncodeNativeValue(Value::Integer(value));
  return vela_native_v1_resume_return(execution, state, 0, &result);
}

TEST(NativeImageLifetimeTest, SuspendedVersionRetainsItsImageUntilResumptionAndCollection) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  auto resource = std::make_shared<AsyncImage>();
  const std::weak_ptr<AsyncImage> lifetime = resource;
  std::shared_ptr<const NativeImage> image(resource, &resource->image);
  auto completer = isolate.scheduler().CreateHostCompleter();
  Value output;
  const RootRange output_root(heap, &output, 1);
  {
    const auto created = NativeInstance::Instantiate(isolate, image);
    ASSERT_TRUE(created);
    NativeExecution execution(**created);
    auto* source = heap.AllocateFuture(completer.future());
    ASSERT_NE(source, nullptr);
    const auto source_value = Value::Object(source);
    const RootRange source_root(heap, &source_value, 1);
    const auto input = EncodeNativeValue(source_value);
    NativeValue result;
    ASSERT_EQ(static_cast<ExecutionStatus>(vela_native_v1_suspend(
                  &execution, 0, ResumePreviousVersion, 0, &input, nullptr, 0, 0, &result)),
              ExecutionStatus::Returned);
    output = DecodeNativeValue(result);
  }
  image.reset();
  resource.reset();
  heap.Collect();
  EXPECT_FALSE(lifetime.expired());
  auto* future = Heap::GetFuture(*output.AsObject());
  ASSERT_NE(future, nullptr);
  constexpr std::int64_t kInput = 39;
  ASSERT_TRUE(completer.Complete(Value::Integer(kInput)));
  ASSERT_TRUE(isolate.scheduler().PumpUntil(*future));
  EXPECT_EQ(future->value().value_or(Value::Integer(0)).AsInteger(), 42);
  heap.Collect();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
}

TEST(NativeImageLifetimeTest, FailedInstantiationReleasesUnpublishedCodeOwner) {
  Isolate isolate;
  const auto before = isolate.layout_counts();
  auto resource = std::make_shared<AsyncImage>();
  const std::weak_ptr<AsyncImage> lifetime = resource;
  resource->functions.front().native_symbol = Text("host.missing");
  {
    const std::shared_ptr<const NativeImage> image(resource, &resource->image);
    const auto created = NativeInstance::Instantiate(isolate, image);
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().status, NativeInstantiationStatus::MissingImport);
  }
  resource.reset();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(isolate.layout_counts(), before);
}

}  // namespace
}  // namespace vela::runtime
