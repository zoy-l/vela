#include "runtime/native/native_instance.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include <gtest/gtest.h>

#include "base/layout_description.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_image_test_support.h"
#include "runtime/strings/string.h"

namespace vela::runtime {
namespace {

using test::EmptyImage;
using test::Table;
using test::Text;

std::int64_t IntegerHost(NativeContext& /*unused*/, std::int64_t value) noexcept {
  ADD_FAILURE() << "instantiation must not execute host code";
  return value;
}

bool BooleanHost(NativeContext& /*unused*/, bool value) noexcept {
  ADD_FAILURE() << "instantiation must not execute host code";
  return value;
}

void SetIntegerImportSignature(NativeFunctionDescription& function,
                               const std::array<NativeCallParameter, 1>& parameters) {
  function.parameters = Table(parameters);
  function.parameter_count = static_cast<std::uint32_t>(parameters.size());
  function.result_type = 0;
  function.flags = kNativeTypedFunction;
}

TEST(NativeInstanceTest, RejectsAbiAndTargetBeforeCreatingLayouts) {
  Isolate isolate;
  const auto initial = isolate.layout_counts();
  auto image = EmptyImage();
  image.abi_identity = Text("different ABI");
  const auto abi = NativeInstance::Instantiate(isolate, image);
  ASSERT_FALSE(abi.has_value());
  EXPECT_EQ(abi.error().status, NativeInstantiationStatus::AbiMismatch);
  image = EmptyImage();
  image.target = Text("different target");
  const auto target = NativeInstance::Instantiate(isolate, image);
  ASSERT_FALSE(target.has_value());
  EXPECT_EQ(target.error().status, NativeInstantiationStatus::TargetMismatch);
  image = EmptyImage();
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
          .kind = static_cast<std::uint32_t>(layout::Type::Kind::Int),
      },
  };
  const std::array parameters{NativeCallParameter{.name = Text("value"), .type = 0}};
  NativeFunctionDescription function;
  function.name = Text("read");
  function.native_symbol = Text("host.read");
  SetIntegerImportSignature(function, parameters);
  const std::array functions{function};
  NativeClassDescription owned;
  owned.name = Text("Owned");
  owned.field_count = 1;
  const std::array classes{owned};
  auto image = EmptyImage();
  image.types = Table(types);
  image.classes = Table(classes);
  image.functions = Table(functions);
  const std::array<std::uint32_t, 1> imports{0};
  image.imported_functions = Table(imports);
  const auto initial_heap_bytes = isolate.heap().allocated_bytes();
  const auto missing = NativeInstance::Instantiate(isolate, image);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().status, NativeInstantiationStatus::MissingImport);
  EXPECT_EQ(isolate.layout_counts(), initial);
  EXPECT_EQ(isolate.heap().allocated_bytes(), initial_heap_bytes);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<&BooleanHost>("host.read")).has_value());
  const auto mismatched = NativeInstance::Instantiate(isolate, image);
  ASSERT_FALSE(mismatched.has_value());
  EXPECT_EQ(mismatched.error().status, NativeInstantiationStatus::ImportSignatureMismatch);
  EXPECT_EQ(isolate.layout_counts(), initial);
  EXPECT_EQ(isolate.heap().allocated_bytes(), initial_heap_bytes);
  Isolate correct;
  ASSERT_TRUE(correct.RegisterNative(NativeFunction::Bind<&IntegerHost>("host.read")).has_value());
  const auto bound = NativeInstance::Instantiate(correct, image);
  ASSERT_TRUE(bound.has_value());
  EXPECT_EQ(&(*bound)->isolate(), &correct);
  EXPECT_EQ((*bound)->layouts().classes.size(), 1U);
}

TEST(NativeInstanceTest, ImportTablePreservesSparseIndicesAndReleasesAccountedStorage) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<IntegerHost>("host.first")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<IntegerHost>("host.second")));
  const auto first = isolate.FindNative("host.first");
  const auto second = isolate.FindNative("host.second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  const std::array types{
      NativeTypeDescription{.kind = static_cast<std::uint32_t>(layout::Type::Kind::Int)},
  };
  const std::array parameters{NativeCallParameter{.name = Text("value"), .type = 0}};
  std::array<NativeFunctionDescription, 4> functions;
  functions[0].name = Text("plainFirst");
  functions[1].name = Text("first");
  functions[1].native_symbol = Text("host.first");
  SetIntegerImportSignature(functions[1], parameters);
  functions[2].name = Text("plainSecond");
  functions[3] = functions[1];
  functions[3].name = Text("second");
  functions[3].native_symbol = Text("host.second");
  auto image = EmptyImage();
  image.types = Table(types);
  image.functions = Table(functions);
  const std::array<std::uint32_t, 2> imported_functions{1, 3};
  image.imported_functions = Table(imported_functions);
  auto& heap = isolate.heap();
  const auto initial_bytes = heap.allocated_bytes();
  auto created = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(created);
  auto instance = std::move(*created);
  const auto* import_data = instance->program().imports;
  ASSERT_NE(import_data, nullptr);
  const auto imports = std::span(import_data, image.functions.size);
  EXPECT_EQ(imports[0], nullptr);
  EXPECT_EQ(imports[1], &first->aot_entry());
  EXPECT_EQ(imports[2], nullptr);
  EXPECT_EQ(imports[3], &second->aot_entry());
  heap.Collect();
  EXPECT_EQ(instance->program().imports, import_data);
  EXPECT_EQ(imports[1], &first->aot_entry());
  EXPECT_EQ(imports[3], &second->aot_entry());
  instance.reset();
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), initial_bytes);
}

TEST(NativeInstanceTest, ReusesCodeIdentityAndSeparatesConstantsAndIsolates) {
  Isolate isolate(1);
  NativeClassDescription type;
  type.name = Text("Box");
  type.field_count = 1;
  const std::array classes{type};
  const std::array strings{Text("rooted")};
  auto image = EmptyImage();
  image.classes = Table(classes);
  image.strings = Table(strings);
  const auto first = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(first.has_value());
  const auto counts = isolate.layout_counts();
  const auto second = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(isolate.layout_counts(), counts);
  EXPECT_EQ((*first)->layouts().classes.front(), (*second)->layouts().classes.front());
  const auto first_constants = test::InstanceConstants(**first);
  const auto second_constants = test::InstanceConstants(**second);
  const auto* first_string = first_constants.front().AsString();
  const auto* second_string = second_constants.front().AsString();
  EXPECT_NE(first_string, second_string);
  isolate.heap().Collect();
  EXPECT_EQ(first_string->value(), "rooted");
  EXPECT_EQ(second_string->value(), "rooted");
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
  auto image = std::make_shared<NativeImage>(EmptyImage());
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
    const auto root_storage = HostRoot::Create(isolate, Value::Object(closure));
    ASSERT_TRUE(root_storage);
    const auto& root = *root_storage;
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

TEST(NativeInstanceTest, ExternalInstanceMustBeReleasedBeforeStop) {
  Isolate isolate;
  const auto image = EmptyImage();
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
  const auto image = EmptyImage();
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
