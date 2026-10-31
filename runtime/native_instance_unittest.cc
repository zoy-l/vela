#include "runtime/native_instance.h"

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#include <gtest/gtest.h>

#include "base/layout_description.h"
#include "runtime/external_resources.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_context.h"
#include "runtime/native_function.h"
#include "runtime/native_image.h"
#include "runtime/native_image_test_support.h"
#include "runtime/rooted_value.h"
#include "runtime/string.h"

namespace vela::runtime {
namespace {

using test::Table;
using test::Text;

NativeImage Image() {
  NativeImage image;
  image.abi_identity = Text(NativeRuntimeAbiIdentity());
  image.target = Text(NativeTargetIdentity());
  return image;
}

std::int64_t IntegerHost(NativeContext& /*unused*/, std::int64_t value) noexcept {
  ADD_FAILURE() << "instantiation must not execute host code";
  return value;
}

bool BooleanHost(NativeContext& /*unused*/, bool value) noexcept {
  ADD_FAILURE() << "instantiation must not execute host code";
  return value;
}

TEST(NativeInstanceTest, RejectsAbiAndTargetBeforeCreatingLayouts) {
  Isolate isolate;
  const auto initial = isolate.layout_counts();
  auto image = Image();
  image.abi_identity = Text("different ABI");
  const auto abi = NativeInstance::Instantiate(isolate, image);
  ASSERT_FALSE(abi.has_value());
  EXPECT_EQ(abi.error().status, NativeInstantiationStatus::AbiMismatch);
  image = Image();
  image.target = Text("different target");
  const auto target = NativeInstance::Instantiate(isolate, image);
  ASSERT_FALSE(target.has_value());
  EXPECT_EQ(target.error().status, NativeInstantiationStatus::TargetMismatch);
  image = Image();
  --image.record_bytes;
  const auto size = NativeInstance::Instantiate(isolate, image);
  ASSERT_FALSE(size.has_value());
  EXPECT_EQ(size.error().status, NativeInstantiationStatus::AbiMismatch);
  EXPECT_EQ(isolate.layout_counts(), initial);
}

TEST(NativeInstanceTest, BindsImportsBeforePublishingAnyLayoutsOrConstants) {
  Isolate isolate;
  const auto initial = isolate.layout_counts();
  const std::array types{
      NativeTypeDescription{
          .kind = static_cast<std::uint32_t>(layout::Type::Kind::Integer),
      },
  };
  const std::array parameters{NativeCallParameter{.name = Text("value"), .type = 0}};
  NativeFunctionDescription function;
  function.name = Text("read");
  function.native_symbol = Text("host.read");
  function.parameters = Table(parameters);
  function.parameter_count = 1;
  function.result_type = 0;
  function.flags = kNativeTypedFunction;
  const std::array functions{function};
  NativeClassDescription owned;
  owned.name = Text("Owned");
  owned.field_count = 1;
  const std::array classes{owned};
  auto image = Image();
  image.types = Table(types);
  image.classes = Table(classes);
  image.functions = Table(functions);
  const auto missing = NativeInstance::Instantiate(isolate, image);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().status, NativeInstantiationStatus::MissingImport);
  EXPECT_EQ(isolate.layout_counts(), initial);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<&BooleanHost>("host.read")).has_value());
  const auto mismatched = NativeInstance::Instantiate(isolate, image);
  ASSERT_FALSE(mismatched.has_value());
  EXPECT_EQ(mismatched.error().status, NativeInstantiationStatus::ImportSignatureMismatch);
  EXPECT_EQ(isolate.layout_counts(), initial);
  Isolate correct;
  ASSERT_TRUE(correct.RegisterNative(NativeFunction::Bind<&IntegerHost>("host.read")).has_value());
  const auto bound = NativeInstance::Instantiate(correct, image);
  ASSERT_TRUE(bound.has_value());
  EXPECT_EQ(&(*bound)->isolate(), &correct);
  EXPECT_EQ((*bound)->layouts().classes.size(), 1U);
}

TEST(NativeInstanceTest, ReusesCodeIdentityAndSeparatesConstantsAndIsolates) {
  Isolate isolate(1);
  NativeClassDescription type;
  type.name = Text("Box");
  type.field_count = 1;
  const std::array classes{type};
  const std::array strings{Text("rooted")};
  auto image = Image();
  image.classes = Table(classes);
  image.strings = Table(strings);
  const auto first = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(first.has_value());
  const auto counts = isolate.layout_counts();
  const auto second = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(isolate.layout_counts(), counts);
  EXPECT_EQ((*first)->layouts().classes.front(), (*second)->layouts().classes.front());
  EXPECT_NE((*first)->constant(0).AsString(), (*second)->constant(0).AsString());
  isolate.heap().Collect();
  EXPECT_EQ((*first)->constant(0).AsString()->value(), "rooted");
  EXPECT_EQ((*second)->constant(0).AsString()->value(), "rooted");
  Isolate other(1);
  const auto third = NativeInstance::Instantiate(other, image);
  ASSERT_TRUE(third.has_value());
  EXPECT_NE((*first)->layouts().classes.front(), (*third)->layouts().classes.front());
}

TEST(NativeInstanceTest, DynamicImageOwnerIsRetainedByReachableCallableAndReleasedAfterCollection) {
  Isolate isolate(1);
  NativeFunctionDescription function;
  function.name = Text("function");
  const std::array functions{function};
  NativeClassDescription callable;
  callable.name = Text("Closure");
  callable.callable_function = 0;
  const std::array classes{callable};
  auto image = std::make_shared<NativeImage>(Image());
  image->classes = Table(classes);
  image->functions = Table(functions);
  const std::weak_ptr<const NativeImage> lifetime = image;
  auto created = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(created);
  auto instance = std::move(*created);
  auto& heap = isolate.heap();
  auto* closure = heap.AllocateInstance(*instance->layouts().classes.front());
  ASSERT_NE(closure, nullptr);
  heap.SetNativeEnvironment(*closure, *instance->state().front().AsObject(),
                            instance->layouts().classes.front()->callable()->function);
  {
    const RootedValue root(isolate, Value::Object(closure));
    instance.reset();
    image.reset();
    heap.Collect();
    EXPECT_FALSE(lifetime.expired());
    auto borrowed = NativeInstance::Borrow(*Heap::NativeEnvironment(*root.value().AsObject()));
    EXPECT_EQ(borrowed.image().functions.values().front().name.text(), "function");
  }
  heap.Collect();
  EXPECT_TRUE(lifetime.expired());
}

TEST(NativeInstanceTest, DynamicImageCacheReusesLiveOwnerWithoutKeepingCodeAlive) {
  Isolate isolate;
  auto image = std::make_shared<NativeImage>(Image());
  const std::weak_ptr<const NativeImage> lifetime = image;
  auto first = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(first);
  auto second = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(second);
  EXPECT_EQ(&(*first)->code_identity(), &(*second)->code_identity());
  first->reset();
  second->reset();
  image.reset();
  isolate.heap().Collect();
  EXPECT_TRUE(lifetime.expired());
}

TEST(NativeInstanceTest, ExternalInstanceMustBeReleasedBeforeStop) {
  Isolate isolate;
  const auto image = Image();
  auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance);
  const auto refused = isolate.TryStop();
  ASSERT_FALSE(refused);
  ASSERT_EQ(refused.error().size(), 1U);
  EXPECT_EQ(refused.error().front().kind, ExternalResourceKind::NativeInstance);
  instance->reset();
  EXPECT_TRUE(isolate.TryStop());
}

TEST(NativeInstanceTest, BorrowedExecutionBlocksStopUntilItsNativeStackReturns) {
  Isolate isolate;
  const auto image = Image();
  auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance);
  {
    auto borrowed = NativeInstance::Borrow(*(*instance)->state().front().AsObject());
    instance->reset();
    NativeExecution execution(borrowed);
    const auto refused = isolate.TryStop();
    ASSERT_FALSE(refused);
    ASSERT_EQ(refused.error().size(), 1U);
    EXPECT_EQ(refused.error().front().kind, ExternalResourceKind::Execution);
  }
  EXPECT_TRUE(isolate.TryStop());
}

}  // namespace
}  // namespace vela::runtime
