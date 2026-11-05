#include <cstdint>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>
#include <vela/types.h>

#include "base/callable.h"
#include "base/layout_description.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_type.h"
#include "runtime/native/native_type_traits.h"

namespace vela {
namespace {
struct FirstNode;
struct SecondNode;
}  // namespace

template <>
struct ScriptTypeTraits<FirstNode> final {
  static constexpr auto kKind = ScriptTypeKind::Class;
  static constexpr std::string_view kModule = "first.vela";
  static constexpr std::string_view kName = "Node";
  using Arguments = std::tuple<>;
};

template <>
struct ScriptTypeTraits<SecondNode> final {
  static constexpr auto kKind = ScriptTypeKind::Class;
  static constexpr std::string_view kModule = "second.vela";
  static constexpr std::string_view kName = "Node";
  using Arguments = std::tuple<>;
};

namespace runtime {
namespace {

using internal::NativeTypeOf;

TEST(NativeTypeTest, NestedContractsPreserveNullabilityNominalsAndCallablePayloads) {
  using FirstCallback = Function<Object<FirstNode>(bool)>;
  using SecondCallback = Function<Object<SecondNode>(bool)>;
  using First = Map<String, Array<Nullable<vela::Future<FirstCallback>>>>;
  using Second = Map<String, Array<Nullable<vela::Future<SecondCallback>>>>;
  const auto first = NativeTypeOf<First>();
  const auto second = NativeTypeOf<Second>();
  EXPECT_NE(first, second);
  EXPECT_NE(first.ContractKey(), second.ContractKey());
  const auto nullable = NativeTypeOf<Nullable<First>>();
  EXPECT_NE(first, nullable);
  EXPECT_NE(first.ContractKey(), nullable.ContractKey());
  EXPECT_EQ(nullable, NativeTypeOf<Nullable<Nullable<First>>>());
}

TEST(NativeTypeTest, GraphIndicesAndDisplayNamesDoNotDefineContractIdentity) {
  using Nested = Array<Map<String, vela::Future<Array<std::int64_t>>>>;
  const auto type = NativeTypeOf<Nested>();
  const auto& root_type = type.type;
  if (!root_type) {
    FAIL() << "expected the nested array type";
  }
  layout::Program imported;
  imported.arrays.push_back({.name = "unrelated", .element = {.kind = layout::Type::Kind::Double}});
  const auto& layouts = type.layouts();
  const auto root = layout::ImportType(imported, layouts, *root_type);
  EXPECT_TRUE(layout::IsSameType(imported, root, layouts, *root_type));
  EXPECT_EQ(layout::TypeKey(imported, root), type.ContractKey());
  imported.arrays[root.index].name = "arbitrary display name";
  EXPECT_TRUE(layout::IsSameType(imported, root, layouts, *root_type));
}

Local<vela::Future<Array<std::int64_t>>> Echo(
    Local<vela::Future<Array<std::int64_t>>> value) noexcept {
  return value;
}

TEST(NativeTypeTest, NativeBindingChecksFuturePayloadBeforeInstantiation) {
  const auto binding = NativeFunction::Bind<Echo>("echo");
  const auto type = NativeTypeOf<vela::Future<Array<std::int64_t>>>();
  const auto& root_type = type.type;
  if (!root_type) {
    FAIL() << "expected the future type";
  }
  auto layouts = type.layouts();
  layouts.types = {*root_type};
  CallSignature signature{
      .parameters = {{.name = "value", .type = 0}},
      .result = 0,
      .parameter_name_order = {},
  };
  EXPECT_TRUE(binding.IsSignatureCompatible(signature, layouts));
  const auto wrong = NativeTypeOf<vela::Future<Array<double>>>();
  const auto& wrong_type = wrong.type;
  if (!wrong_type) {
    FAIL() << "expected the mismatched future type";
  }
  layouts = wrong.layouts();
  layouts.types = {*wrong_type};
  EXPECT_FALSE(binding.IsSignatureCompatible(signature, layouts));
}

TEST(NativeTypeTest, FunctionParametersResultsAndFlagsParticipateInContract) {
  using Callback = Function<vela::Future<Array<String>>(Set<std::int64_t>)>;
  const auto expected = NativeTypeOf<Callback>();
  const auto& expected_type = expected.type;
  if (!expected_type) {
    FAIL() << "expected the callback type";
  }
  const auto& expected_layouts = expected.layouts();
  auto changed = expected_layouts;
  auto& signature = changed.signatures[expected_type->index];
  signature.parameters.front().optional = true;
  EXPECT_FALSE(layout::IsSameType(changed, *expected_type, expected_layouts, *expected_type));
  EXPECT_NE(layout::TypeKey(changed, *expected_type), expected.ContractKey());
}

}  // namespace
}  // namespace runtime
}  // namespace vela
