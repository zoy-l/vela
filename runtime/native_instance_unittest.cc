#include "runtime/native_instance.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "base/layout_description.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/native_image.h"
#include "runtime/string.h"

namespace vela::runtime {
namespace {

NativeText Text(std::string_view text) {
  return {.data = text.data(), .size = text.size()};
}

template <class T, std::size_t Size>
NativeTable<T> Table(const std::array<T, Size>& values) {
  return {.data = values.data(), .size = values.size()};
}

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

}  // namespace
}  // namespace vela::runtime
