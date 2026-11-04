#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/native_binding.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_class.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_host_type.h"

namespace vela::runtime {
namespace {

struct RegisteredHost final {};
struct SecondHost final {};

bool NativeFlag() noexcept {
  return true;
}

}  // namespace

template <>
struct NativeClassTraits<RegisteredHost> final {
  static constexpr std::string_view kKey = "test.Host";
};

template <>
struct NativeClassTraits<SecondHost> final {
  static constexpr std::string_view kKey = "test.Second";
};

namespace {

NativeClass HostWithStaticMethod() {
  std::vector<NativeClassMember> members;
  members.push_back({
      .kind = NativeMemberKind::Static,
      .name = "Flag",
      .parameter_names = {},
      .function = NativeFunction::Bind<&NativeFlag>("test.Host::static:Flag"),
  });
  return NativeClass::Define<RegisteredHost>(std::move(members));
}

TEST(NativeClassRegistrationTest, PublishesWholeModuleAndFreezesImmutableDescriptions) {
  Isolate isolate;
  std::vector<NativeClass> module;
  module.push_back(HostWithStaticMethod());
  module.push_back(NativeClass::Define<SecondHost>({}));
  ASSERT_TRUE(isolate.RegisterNativeModule(std::move(module)).has_value());
  const auto* type = isolate.FindNativeClass("test.Host");
  ASSERT_NE(type, nullptr);
  const auto& description = *type->description;
  EXPECT_EQ(description.size, sizeof(RegisteredHost));
  EXPECT_EQ(description.alignment, alignof(RegisteredHost));
  ASSERT_EQ(description.members.size(), 1U);
  const auto function = isolate.FindNative("test.Host::static:Flag");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(function.get(), &description.members.front().function);
  const auto bindings = isolate.FreezeBindings();
  Isolate second(bindings);
  const auto* second_type = second.FindNativeClass("test.Host");
  ASSERT_NE(second_type, nullptr);
  EXPECT_EQ(second_type->description, type->description);
  EXPECT_NE(second_type->layout, type->layout);
  EXPECT_EQ(second.FindNative("test.Host::static:Flag"), function);
  const auto frozen = second.RegisterNativeModule({});
  ASSERT_FALSE(frozen);
  EXPECT_EQ(frozen.error(), NativeRegistrationError::BindingsFrozen);
}

TEST(NativeClassRegistrationTest, RejectsExistingSymbolsWithoutPublishingCandidateClasses) {
  Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<&NativeFlag>("test.Host::static:Flag"))
                  .has_value());
  std::vector<NativeClass> module;
  module.push_back(NativeClass::Define<SecondHost>({}));
  module.push_back(HostWithStaticMethod());
  const auto registration = isolate.RegisterNativeModule(std::move(module));
  ASSERT_FALSE(registration);
  EXPECT_EQ(registration.error(), NativeRegistrationError::DuplicateName);
  EXPECT_EQ(isolate.FindNativeClass("test.Host"), nullptr);
  EXPECT_EQ(isolate.FindNativeClass("test.Second"), nullptr);
  EXPECT_NE(isolate.FindNative("test.Host::static:Flag"), nullptr);
}

TEST(NativeClassRegistrationTest, RejectsDuplicateTypesAndMalformedMemberKeysAtomically) {
  Isolate isolate;
  std::vector<NativeClass> duplicate;
  duplicate.push_back(HostWithStaticMethod());
  duplicate.push_back(NativeClass::Define<RegisteredHost>({}));
  const auto registration = isolate.RegisterNativeModule(std::move(duplicate));
  ASSERT_FALSE(registration);
  EXPECT_EQ(registration.error(), NativeRegistrationError::DuplicateName);
  EXPECT_EQ(isolate.FindNativeClass("test.Host"), nullptr);
  EXPECT_EQ(isolate.FindNative("test.Host::static:Flag"), nullptr);

  auto malformed = HostWithStaticMethod();
  malformed.members.front().name = "Other";
  std::vector<NativeClass> candidate;
  candidate.push_back(std::move(malformed));
  const auto invalid = isolate.RegisterNativeModule(std::move(candidate));
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), NativeRegistrationError::InvalidMember);
  EXPECT_EQ(isolate.FindNativeClass("test.Host"), nullptr);
}

}  // namespace
}  // namespace vela::runtime
