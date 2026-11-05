#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/execution_status.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"
#include "vela/collections.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/type.h"
#include "vela/types.h"

namespace vela {
namespace {
struct NodeTag;
struct ReadableTag;
struct ColorTag;
}  // namespace

template <>
struct ScriptTypeTraits<NodeTag> final {
  static constexpr auto kKind = ScriptTypeKind::Class;
  static constexpr std::string_view kName = "Node";
  static constexpr std::string_view kModule = "main.vela";
  using Arguments = std::tuple<>;
};
template <>
struct ScriptTypeTraits<ReadableTag> final {
  static constexpr auto kKind = ScriptTypeKind::Interface;
  static constexpr std::string_view kName = "Readable";
  static constexpr std::string_view kModule = "main.vela";
  using Arguments = std::tuple<>;
};
template <>
struct ScriptTypeTraits<ColorTag> final {
  static constexpr auto kKind = ScriptTypeKind::Enum;
  static constexpr std::string_view kName = "Color";
  static constexpr std::string_view kModule = "main.vela";
  using Arguments = std::tuple<>;
};

namespace compiler {
namespace {

TEST(HostNominalTypeTest, SameNamedDeclarationsBindOnlyToTheirExplicitProgram) {
  runtime::Isolate isolate(1);
  auto first = test::CompileCleanSource(R"(
interface Readable { read(): int; }
class Node implements Readable {
  value: int;
  read(): int { return this.value; }
}
func main(): Node { return Node(value: 42); }
)",
                                        isolate);
  auto second = test::CompileCleanSource(R"(
interface Readable { read(): int; }
class Node implements Readable {
  value: int;
  read(): int { return this.value; }
}
func main(): Node { return Node(value: 1); }
)",
                                         isolate);
  ASSERT_TRUE(first) << first.error();
  ASSERT_TRUE(second) << second.error();
  const auto unbound = Type<Object<NodeTag>>::Bind(isolate);
  ASSERT_FALSE(unbound);
  EXPECT_EQ(unbound.error().status, ExecutionStatus::InvalidEntry);
  const auto first_type = Type<Array<Object<NodeTag>>>::Bind(**first);
  const auto second_type = Type<Array<Object<NodeTag>>>::Bind(**second);
  const auto readable = Type<Object<ReadableTag>>::Bind(**first);
  ASSERT_TRUE(first_type);
  ASSERT_TRUE(second_type);
  ASSERT_TRUE(readable);
  HandleScope scope(isolate);
  const auto first_array = CreateArray(scope, *first_type);
  const auto second_array = CreateArray(scope, *second_type);
  ASSERT_TRUE(first_array);
  ASSERT_TRUE(second_array);
  const auto result = test::Execute(**first);
  ASSERT_TRUE(result.Succeeded());
  const auto value = vela::internal::HandleAccess::Root<Value>(scope, result.value);
  const auto object_type = Type<Object<NodeTag>>::Bind(**first);
  ASSERT_TRUE(value);
  ASSERT_TRUE(object_type);
  const auto object = object_type->Cast(scope, *value);
  ASSERT_TRUE(object);
  ASSERT_TRUE(first_array->Push(scope, *object));
  const auto rejected = second_array->Push(scope, *object);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, ExecutionStatus::TypeError);
  const auto view = readable->Cast(scope, object->ToValue());
  ASSERT_TRUE(view);
  const auto wrong = second_type->Cast(scope, first_array->ToValue());
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error().status, ExecutionStatus::InvalidCast);
  first->reset();
  second->reset();
  isolate.heap().Collect();
  const auto child = first_array->Get(scope, 0);
  ASSERT_TRUE(child);
  EXPECT_TRUE(object_type->Cast(scope, child->ToValue()));
}

using Color = Enum<ColorTag>;
using ColorMap = Map<Color, Nullable<Color>>;

Result<Local<ColorMap>> MakeColorMap(runtime::NativeContext& context,
                                     Local<Color> first,
                                     Local<Color> second) noexcept {
  auto& scope = context.handles();
  auto type = context.BindType<ColorMap>();
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  auto values = CreateMap(scope, *type);
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }
  auto null = values->Set(scope, first, std::nullopt);
  if (!null) {
    return std::unexpected(std::move(null.error()));
  }
  auto inserted = values->Set(scope, second, first);
  if (!inserted) {
    return std::unexpected(std::move(inserted.error()));
  }
  auto keys = values->Keys(scope);
  if (!keys) {
    return std::unexpected(std::move(keys.error()));
  }
  auto set_type = context.BindType<Set<Color>>();
  if (!set_type) {
    return std::unexpected(std::move(set_type.error()));
  }
  auto set = CreateSet(scope, *set_type);
  if (!set) {
    return std::unexpected(std::move(set.error()));
  }
  auto key = keys->Get(scope, 1);
  if (!key) {
    return std::unexpected(std::move(key.error()));
  }
  auto added = set->Add(scope, *key);
  if (!added) {
    return std::unexpected(std::move(added.error()));
  }
  const auto has = set->Has(scope, second);
  if (!has || !*has) {
    return std::unexpected(Error{
        .status = ExecutionStatus::NativeFailure,
        .message = "typed enum key did not retain its identity",
        .stack_trace = {},
    });
  }
  context.isolate().heap().Collect();
  auto found = values->Get(scope, second);
  if (!found) {
    return std::unexpected(std::move(found.error()));
  }
  if (!*found || !**found) {
    return std::unexpected(Error{
        .status = ExecutionStatus::NativeFailure,
        .message = "typed enum value was lost",
        .stack_trace = {},
    });
  }
  return *values;
}

TEST(HostNominalTypeTest, HostCreatedEnumMapsAndSetsShareTheScriptEnumIdentity) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<MakeColorMap>("make_colors")));
  const auto instance = test::CompileCleanSource(R"(
enum Color { red, green, blue }
external func make_colors(first: Color, second: Color): Map<Color, Color?>;
func main(): int {
  const values = make_colors(Color.red, Color.green);
  if (values.size != 2 || values.get(Color.red) != null ||
      values.get(Color.blue) != null || values.get(Color.green) != Color.red) { return -1; }
  const keys = values.keys();
  return keys[0] == Color.red && keys[1] == Color.green ? 42 : -2;
}
)",
                                                 isolate);
  ASSERT_TRUE(instance) << instance.error();
  const auto result = test::ExecuteInteger(**instance);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

using ObjectArrays = Map<Array<std::int64_t>, Array<std::int64_t>>;

Result<Local<ObjectArrays>> ShareArrays(runtime::NativeContext& context,
                                        Local<Array<std::int64_t>> values) noexcept {
  auto& scope = context.handles();
  auto map = CreateMap<Array<std::int64_t>, Array<std::int64_t>>(scope);
  if (!map) {
    return std::unexpected(std::move(map.error()));
  }
  auto inserted = map->Set(scope, values, values);
  if (!inserted) {
    return std::unexpected(std::move(inserted.error()));
  }
  return *map;
}

TEST(HostNominalTypeTest, HostCreatedMapRetainsTheOriginalScriptArrayAsKeyAndValue) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<ShareArrays>("share_arrays")));
  const auto instance = test::CompileCleanSource(R"(
external func share_arrays(values: int[]): Map<int[], int[]>;
func main(): int {
  const values = [40];
  const map = share_arrays(values);
  const found = map.get(values);
  if (found == null || map.has([40])) { return -1; }
  found[0] += 2;
  return values[0];
}
)",
                                                 isolate);
  ASSERT_TRUE(instance) << instance.error();
  const auto result = test::ExecuteInteger(**instance);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

}  // namespace
}  // namespace compiler
}  // namespace vela
