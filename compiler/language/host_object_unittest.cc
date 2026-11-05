#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/execution_status.h"

#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/object.h"
#include "vela/entry.h"
#include "vela/handles.h"
#include "vela/strings.h"
#include "vela/type.h"
#include "vela/types.h"

namespace vela {
namespace {
struct CounterTag;
struct CounterViewTag;
struct SecretTag;
struct ChoiceTag;
}

template <>
struct ScriptTypeTraits<CounterTag> final {
  static constexpr auto kKind = ScriptTypeKind::Class;
  static constexpr std::string_view kModule = "main.vela";
  static constexpr std::string_view kName = "Counter";
  using Arguments = std::tuple<>;
};
template <>
struct ScriptTypeTraits<CounterViewTag> final {
  static constexpr auto kKind = ScriptTypeKind::Interface;
  static constexpr std::string_view kModule = "main.vela";
  static constexpr std::string_view kName = "CounterView";
  using Arguments = std::tuple<>;
};
template <>
struct ScriptTypeTraits<SecretTag> final {
  static constexpr auto kKind = ScriptTypeKind::Class;
  static constexpr std::string_view kModule = "main.vela";
  static constexpr std::string_view kName = "Secret";
  using Arguments = std::tuple<>;
};
template <>
struct ScriptTypeTraits<ChoiceTag> final {
  static constexpr auto kKind = ScriptTypeKind::Enum;
  static constexpr std::string_view kModule = "main.vela";
  static constexpr std::string_view kName = "Choice";
  using Arguments = std::tuple<>;
};

namespace compiler {
namespace {

constexpr std::int64_t kInitialCounter = 41;

using GraphCallback = Function<std::int64_t(bool)>;
struct RetainedGraph final {
  Persistent<Array<std::int64_t>> values;
  Persistent<Object<CounterTag>> object;
  Persistent<GraphCallback> callback;
};

RetainedGraph*& GraphSink() {
  static thread_local RetainedGraph* sink = nullptr;
  return sink;
}

Result<void> KeepGraph(Local<Array<std::int64_t>> values,
                       Local<Object<CounterTag>> object,
                       Local<GraphCallback> callback) noexcept {
  auto kept_values = values.Persist();
  if (!kept_values) {
    return std::unexpected(std::move(kept_values.error()));
  }
  auto kept_object = object.Persist();
  if (!kept_object) {
    return std::unexpected(std::move(kept_object.error()));
  }
  auto kept_callback = callback.Persist();
  if (!kept_callback) {
    return std::unexpected(std::move(kept_callback.error()));
  }
  auto& graph = *GraphSink();
  graph.values = std::move(*kept_values);
  graph.object = std::move(*kept_object);
  graph.callback = std::move(*kept_callback);
  return {};
}

TEST(HostObjectTest, ImportedGraphSurvivesItsInstanceAndRetiresAllOwnersAfterTheLastHandle) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<KeepGraph>("keep")));
  const auto initial_layouts = isolate.layout_counts();
  auto compiler = jit::Compiler::Create();
  ASSERT_TRUE(compiler.has_value());
  RetainedGraph graph;
  std::optional<Type<Object<CounterTag>>> type;
  {
    auto compiled = jit::CompileSource(**compiler, R"(
export class Counter { value: int = 0; }
external func keep(values: int[], object: Counter, callback: (bool) => int): void;
export func main(): void {
  const values = [1];
  const counter = Counter();
  let calls = 0;
  keep(values, counter, (fail: bool): int => {
    calls += 1;
    if (fail) { throw counter; }
    return values[0] + counter.value + calls;
  });
}
)");
    ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
    auto instance = runtime::NativeInstance::Instantiate(isolate, std::move(compiled.code.image));
    ASSERT_TRUE(instance.has_value());
    auto bound = Type<Object<CounterTag>>::Bind(**instance);
    ASSERT_TRUE(bound.has_value());
    type = std::move(*bound);
    GraphSink() = &graph;
    const auto result = test::Execute(**instance);
    GraphSink() = nullptr;
    ASSERT_TRUE(result.Succeeded()) << result.message;
  }
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().images, 1U);
  EXPECT_FALSE(isolate.TryStop());
  {
    HandleScope scope(isolate);
    auto values = graph.values.ToLocal(scope);
    auto object = graph.object.ToLocal(scope);
    auto callback = graph.callback.ToLocal(scope);
    ASSERT_TRUE(values.has_value());
    ASSERT_TRUE(object.has_value());
    ASSERT_TRUE(callback.has_value());
    graph = {};
    ASSERT_TRUE(type.has_value());
    ASSERT_TRUE(values->Set(scope, 0, 2));
    ASSERT_TRUE(type->Set<std::int64_t>(scope, *object, "value", 39));
    isolate.heap().Collect();
    const auto first = callback->Call(scope, false);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 42);
    {
      auto failed = callback->Call(scope, true);
      ASSERT_FALSE(failed.has_value());
      EXPECT_EQ(failed.error().status, ExecutionStatus::Thrown);
      isolate.heap().Collect();
      const auto payload = failed.error().payload.ToLocal(scope);
      ASSERT_TRUE(payload.has_value());
      EXPECT_EQ(vela::internal::HandleAccess::Read(*payload).AsObject(),
                vela::internal::HandleAccess::Read(*object).AsObject());
      const auto resumed = callback->Call(scope, false);
      ASSERT_TRUE(resumed.has_value());
      EXPECT_EQ(*resumed, 44);
    }
  }
  type.reset();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
  EXPECT_EQ(isolate.layout_counts(), initial_layouts);
  EXPECT_TRUE(isolate.ReloadBlockers().empty());
  EXPECT_EQ((*compiler)->resource_counts().images, 0U);
  EXPECT_TRUE(isolate.TryStop());
}

TEST(HostObjectTest, PublicMembersRetainProgramAndExecutePropertyAndBoundMethodSemantics) {
  runtime::Isolate isolate(1);
  std::optional<Type<Object<CounterTag>>> type;
  Persistent<Object<CounterTag>> saved;
  {
    const auto compiled = test::CompileCleanSource(R"(
interface CounterView { add(value: int): int; }
class Counter implements CounterView {
  value: int = 40;
  private secret: int = 99;
  get next(): int { this.value += 1; return this.value; }
  get readonly(): int { return this.value; }
  set next(value: int): void { this.value = value + 1; }
  add(value: int): int { this.value += value; return this.value; }
}
export func create(): Counter { return Counter(); }
)",
                                                   isolate);
    ASSERT_TRUE(compiled) << compiled.error();
    auto bound = Type<Object<CounterTag>>::Bind(**compiled);
    ASSERT_TRUE(bound);
    type = std::move(*bound);
    const auto create = Entry<Object<CounterTag>()>::Bind(**compiled, "create");
    ASSERT_TRUE(create);
    HandleScope scope(isolate);
    const auto object = create->Call(scope);
    ASSERT_TRUE(object);
    auto root = object->Persist();
    ASSERT_TRUE(root);
    saved = std::move(*root);
  }
  isolate.heap().Collect();
  {
    HandleScope scope(isolate);
    const auto object = saved.ToLocal(scope);
    ASSERT_TRUE(object);
    saved.Reset();
    const auto next = type->Get<std::int64_t>(scope, *object, "next");
    ASSERT_TRUE(next);
    EXPECT_EQ(*next, 41);
    EXPECT_FALSE(type->Get<std::int64_t>(scope, *object, "secret"));
    EXPECT_FALSE(type->Set<std::int64_t>(scope, *object, "readonly", 0));
    ASSERT_TRUE(type->Set<std::int64_t>(scope, *object, "next", 39));
    const auto method = type->Get<Function<std::int64_t(std::int64_t)>>(scope, *object, "add");
    ASSERT_TRUE(method);
    isolate.heap().Collect();
    const auto result = method->Call(scope, std::int64_t{2});
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 42);
    EXPECT_FALSE(type->Set<String>(scope, *object, "value", *CreateString(scope, "wrong")));
    const auto value = type->Get<std::int64_t>(scope, *object, "value");
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 42);
  }
  type.reset();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(HostObjectTest, EmptyNominalArrayRetainsMetadataAfterItsTypeAndCodeOwnerAreReleased) {
  runtime::Isolate isolate(1);
  const auto before = isolate.layout_counts();
  auto compiler = jit::Compiler::Create();
  ASSERT_TRUE(compiler.has_value());
  Persistent<Array<Object<CounterTag>>> saved;
  {
    auto compiled = jit::CompileSource(**compiler, R"(
export class Counter { value: int = 42; }
export func empty(): Counter[] { return []; }
)");
    ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
    const auto instance =
        runtime::NativeInstance::Instantiate(isolate, std::move(compiled.code.image));
    ASSERT_TRUE(instance.has_value());
    const auto type = Type<Array<Object<CounterTag>>>::Bind(**instance);
    ASSERT_TRUE(type.has_value());
    HandleScope scope(isolate);
    const auto array = CreateArray(scope, *type);
    ASSERT_TRUE(array.has_value());
    auto persistent = array->Persist();
    ASSERT_TRUE(persistent.has_value());
    saved = std::move(*persistent);
  }
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().images, 0U);
  EXPECT_EQ(isolate.heap().object_count(), 1U);
  {
    HandleScope scope(isolate);
    const auto array = saved.ToLocal(scope);
    ASSERT_TRUE(array.has_value());
    EXPECT_EQ(array->Size(), 0);
    const auto value = vela::internal::HandleAccess::Read(*array);
    const auto* nominal = runtime::Heap::TypeOf(value).array->element.nominal;
    ASSERT_NE(nominal, nullptr);
    ASSERT_NE(nominal->fixed_layout(), nullptr);
    EXPECT_EQ(nominal->fixed_layout()->field_count(), 1U);
    EXPECT_FALSE(array->Get(scope, 0));
    saved.Reset();
    isolate.heap().Collect();
    EXPECT_EQ(array->Size(), 0);
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.layout_counts(), before);
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(HostObjectTest, InterfaceAndDynamicViewsUseTheSameCheckedPublicMemberPath) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(R"(
interface CounterView { add(value: int): int; }
class Counter implements CounterView {
  value: int = 40;
  add(value: int): int { this.value += value; return this.value; }
}
export func create(): Counter { return Counter(); }
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto create = Entry<Object<CounterTag>()>::Bind(**compiled, "create");
  const auto view = Type<Object<CounterViewTag>>::Bind(**compiled);
  const auto dynamic = Type<Value>::Bind(**compiled);
  ASSERT_TRUE(create);
  ASSERT_TRUE(view);
  ASSERT_TRUE(dynamic);
  HandleScope scope(isolate);
  const auto object = create->Call(scope);
  ASSERT_TRUE(object);
  const auto converted = view->Cast(scope, object->ToValue());
  ASSERT_TRUE(converted);
  EXPECT_FALSE(view->Get<std::int64_t>(scope, *converted, "value"));
  const auto method = view->Get<Function<std::int64_t(std::int64_t)>>(scope, *converted, "add");
  ASSERT_TRUE(method);
  const auto result = method->Call(scope, std::int64_t{2});
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 42);
  const auto value = dynamic->Get<std::int64_t>(scope, object->ToValue(), "value");
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 42);
}

TEST(HostObjectTest, PublicConstructionRunsDefaultsInheritanceAndUserConstructors) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(R"(
let calls = 0;
func seed(): int { calls++; return 40; }
class Base { base: int = seed(); }
export class Counter extends Base {
  value: int;
  constructor(value: int) { this.value = value + this.base; }
}
class Secret { private constructor() {} }
export func count(): int { return calls; }
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto type = Type<Object<CounterTag>>::Bind(**compiled);
  const auto secret = Type<Object<SecretTag>>::Bind(**compiled);
  const auto count = Entry<std::int64_t()>::Bind(**compiled, "count");
  ASSERT_TRUE(type);
  ASSERT_TRUE(secret);
  ASSERT_TRUE(count);
  HandleScope scope(isolate);
  const auto before = count->Call(scope);
  ASSERT_TRUE(before);
  EXPECT_EQ(*before, 0);
  EXPECT_FALSE(type->Construct(scope));
  EXPECT_FALSE(secret->Construct(scope));
  auto object = type->Construct(scope, NamedArgument<std::int64_t>{.name = "value", .value = 2});
  ASSERT_TRUE(object) << static_cast<int>(object.error().status);
  const auto value = type->Get<std::int64_t>(scope, *object, "value");
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 42);
  const auto after = count->Call(scope);
  ASSERT_TRUE(after);
  EXPECT_EQ(*after, 1);
}

TEST(HostObjectTest, GeneratedConstructionAndEnumsUseTheirDeclaredPublicContracts) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(R"(
enum Choice { first, second }
export class Counter { value: int = 42; name: string = 'default'; }
export func choice(value: Choice): bool { return value == Choice.second; }
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto type = Type<Object<CounterTag>>::Bind(**compiled);
  const auto enumeration = Type<Enum<ChoiceTag>>::Bind(**compiled);
  const auto check = Entry<bool(Enum<ChoiceTag>)>::Bind(**compiled, "choice");
  ASSERT_TRUE(type);
  ASSERT_TRUE(enumeration);
  ASSERT_TRUE(check);
  HandleScope scope(isolate);
  auto object = type->Construct(scope);
  ASSERT_TRUE(object);
  auto value = type->Get<std::int64_t>(scope, *object, "value");
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 42);
  object = type->Construct(scope,
                           NamedArgument<std::int64_t>{.name = "value", .value = kInitialCounter});
  ASSERT_TRUE(object);
  value = type->Get<std::int64_t>(scope, *object, "value");
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 41);
  const auto member = enumeration->Member(scope, "second");
  ASSERT_TRUE(member);
  isolate.heap().Collect();
  const auto result = check->Call(scope, *member);
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result);
  EXPECT_FALSE(enumeration->Member(scope, "missing"));
}

}  // namespace
}  // namespace compiler
}  // namespace vela
