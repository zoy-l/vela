#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>
#include <vela/handles.h>
#include <vela/host.h>
#include <vela/types.h>

#include "base/execution_status.h"
#include "compiler/test_support/execution_expectations.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"
#include "vela/entry.h"
#include "vela/error.h"
#include "vela/type.h"

namespace vela::compiler {
namespace {

using NestedIntegers = Array<Nullable<Array<std::int64_t>>>;

std::int64_t LocalRootCount(runtime::NativeContext& context) noexcept {
  const auto resources = context.isolate().ReloadBlockers();
  return static_cast<std::int64_t>(std::ranges::count(
      resources, runtime::ExternalResourceKind::RootRange, &runtime::ExternalResourceInfo::kind));
}

Result<std::int64_t> ScopedTextLength(runtime::NativeContext& context) noexcept {
  auto text = CreateString(context.handles(), "42");
  if (!text) {
    return std::unexpected(std::move(text.error()));
  }
  context.isolate().heap().Collect();
  return StringLength(*text);
}

TEST(HostTypesTest, ScalarContextCreatesRootsOnlyWhenTheHostUsesHandles) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<LocalRootCount>("roots")));
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<ScopedTextLength>("textLength")));
  const auto compiled = test::CompileCleanSource(R"(
external func roots(): int;
external func textLength(): int;
func readRoots(): int { return roots(); }
func main(): int { return textLength(); }
)",
                                                 isolate);
  ASSERT_TRUE(compiled.has_value()) << compiled.error();
  const auto before = test::Execute(**compiled, "readRoots");
  ASSERT_TRUE(before.Succeeded()) << before.message;
  EXPECT_EQ(before.value.AsInteger(), 0);
  const auto length = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(length.has_value()) << length.error();
  EXPECT_EQ(*length, 2);
  const auto after = test::Execute(**compiled, "readRoots");
  ASSERT_TRUE(after.Succeeded()) << after.message;
  EXPECT_EQ(after.value.AsInteger(), 0);
}

template <class T>
Local<T> EchoBuiltin(runtime::NativeContext& context, Local<T> value) noexcept {
  context.isolate().heap().Collect();
  return value;
}

TEST(HostTypesTest, BuiltinReferencesKeepIdentityAndTheirOwnersAcrossTheImportBoundary) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<EchoBuiltin<MutableString>>("builder")));
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<EchoBuiltin<ByteArray>>("bytes")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<EchoBuiltin<ByteData>>("data")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<EchoBuiltin<RegExp>>("regexp")));
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<EchoBuiltin<RegExpMatch>>("echoMatch")));
  ASSERT_TRUE(isolate.RegisterNative(
      runtime::NativeFunction::Bind<EchoBuiltin<Completer<std::int64_t>>>("completer")));
  const auto compiled = test::CompileCleanSource(R"(
external func builder(value: String): String;
external func bytes(value: ByteArray): ByteArray;
external func data(value: ByteData): ByteData;
external func regexp(value: RegExp): RegExp;
external func echoMatch(value: RegExpMatch): RegExpMatch;
external func completer(value: Completer<int>): Completer<int>;
func main(): int {
  const text = String();
  text.append('4');
  builder(text).append('2');
  const storage = ByteArray(4);
  const view = ByteData.view(bytes(storage));
  data(view).setInt32(0, 42);
  const pattern = RegExp('42');
  const found = regexp(pattern).firstMatch(text.toString());
  if (found == null) { return -1; }
  const returned = echoMatch(found);
  const completion = Completer<int>();
  const endpoint = completer(completion);
  endpoint.complete(42);
  return text.toString() == '42' && storage[3] == 42 && returned == found &&
      returned.pattern == pattern && returned[0] == '42' && endpoint == completion ? 42 : -2;
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  test::ExpectInteger(**compiled);
}

TEST(HostTypesTest, IsolateTypeResolutionDoesNotMakeProgramContainerMetadataPermanent) {
  runtime::Isolate isolate(1);
  const auto before = isolate.layout_counts();
  Persistent<Array<std::int64_t>> saved;
  {
    const auto compiled =
        test::CompileCleanSource("export func create(): int[] { return []; }", isolate);
    ASSERT_TRUE(compiled.has_value()) << compiled.error();
    const auto entry = Entry<Array<std::int64_t>()>::Bind(**compiled, "create");
    ASSERT_TRUE(entry.has_value());
    HandleScope scope(isolate);
    const auto array = entry->Call(scope);
    ASSERT_TRUE(array.has_value());
    const auto type = Type<Array<std::int64_t>>::Bind(isolate);
    ASSERT_TRUE(type.has_value());
    auto retained = array->Persist();
    ASSERT_TRUE(retained.has_value());
    saved = std::move(*retained);
  }
  isolate.heap().Collect();
  {
    HandleScope scope(isolate);
    const auto array = saved.ToLocal(scope);
    ASSERT_TRUE(array.has_value());
    saved.Reset();
    ASSERT_TRUE(array->Push(scope, 42));
    const auto value = array->Get(scope, 0);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 42);
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
  EXPECT_EQ(isolate.layout_counts(), before);
}

std::optional<std::int64_t> Increment(std::optional<std::int64_t> value) noexcept {
  return value ? std::optional(*value + 1) : std::nullopt;
}

std::optional<double> Half(std::optional<double> value) noexcept {
  return value ? std::optional(*value / 2) : std::nullopt;
}

std::optional<bool> Flip(std::optional<bool> value) noexcept {
  return value ? std::optional(!*value) : std::nullopt;
}

using Transform = Function<std::int64_t(std::int64_t)>;
using RetainedCallback = Function<std::int64_t()>;

Result<void> Apply(runtime::NativeContext& context,
                   Local<Array<std::int64_t>> values,
                   Local<Transform> transform) noexcept {
  auto& scope = context.handles();
  auto first = values.Get(scope, 0);
  if (!first) {
    return std::unexpected(std::move(first.error()));
  }
  auto next = transform.Call(scope, *first);
  context.isolate().heap().Collect();
  if (!next) {
    return std::unexpected(std::move(next.error()));
  }
  return values.Set(scope, 0, *next);
}

Persistent<RetainedCallback>*& RetainedCallbackSink() {
  static thread_local Persistent<RetainedCallback>* sink = nullptr;
  return sink;
}

class CallbackSink final {
 public:
  explicit CallbackSink(Persistent<RetainedCallback>& value) { RetainedCallbackSink() = &value; }
  ~CallbackSink() { RetainedCallbackSink() = nullptr; }
  CallbackSink(const CallbackSink&) = delete;
  CallbackSink& operator=(const CallbackSink&) = delete;
  CallbackSink(CallbackSink&&) = delete;
  CallbackSink& operator=(CallbackSink&&) = delete;
};

Result<void> Keep(Local<RetainedCallback> callback) noexcept {
  auto saved = callback.Persist();
  if (!saved) {
    return std::unexpected(std::move(saved.error()));
  }
  *RetainedCallbackSink() = std::move(*saved);
  return {};
}

Result<std::int64_t> Forward(runtime::NativeContext& context,
                             Local<RetainedCallback> callback) noexcept {
  return callback.Call(context.handles());
}

TEST(HostTypesTest, CallbackReentrySharesTheLogicalCallDepthLimit) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Forward>("forward")));
  const auto compiled = test::CompileCleanSource(R"(
external func forward(callback: () => int): int;
func recurse(): int { return forward(recurse); }
func main(): int {
  try { return recurse(); }
  catch (error: Error) { return error.code == 'StackOverflow' ? 42 : -1; }
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  test::ExpectInteger(**compiled);
}

TEST(HostTypesTest, HostCallsTypedScriptCallbackAndMutatesOriginalArray) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Apply>("apply")));
  const auto compiled = test::CompileCleanSource(R"(
external func apply(values: int[], transform: (int) => int): void;
func main(): int {
  const values = [41];
  apply(values, value => value + 1);
  return values[0];
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  test::ExpectInteger(**compiled);
}

TEST(HostTypesTest, ReentryRestoresOriginalScriptExceptionInsteadOfWrappingNativeFailure) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Apply>("apply")));
  const auto compiled = test::CompileCleanSource(R"(
external func apply(values: int[], transform: (int) => int): void;
class Failure { value: int; }
func main(): int {
  const failure = Failure(value: 42);
  const values = [1];
  try { apply(values, (value: int): int => { throw failure; }); }
  catch (error: Failure) { return error == failure && values[0] == 1 ? error.value : -1; }
  return -2;
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  test::ExpectInteger(**compiled);
}

TEST(HostTypesTest, PersistentCallbackKeepsCapturedProgramAfterInitialInstanceIsDestroyed) {
  runtime::Isolate isolate(1);
  Persistent<RetainedCallback> saved;
  const CallbackSink sink(saved);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Keep>("keep")));
  {
    const auto compiled = test::CompileCleanSource(R"(
external func keep(callback: () => int): void;
func main(): void {
  let value = 40;
  keep((): int => { value += 1; return value; });
}
)",
                                                   isolate);
    ASSERT_TRUE(compiled) << compiled.error();
    ASSERT_TRUE(test::Execute(**compiled, "main").Succeeded());
  }
  isolate.heap().Collect();
  {
    HandleScope scope(isolate);
    const auto callback = saved.ToLocal(scope);
    ASSERT_TRUE(callback);
    saved.Reset();
    isolate.heap().Collect();
    const auto first = callback->Call(scope);
    ASSERT_TRUE(first);
    EXPECT_EQ(*first, 41);
    const auto second = callback->Call(scope);
    ASSERT_TRUE(second);
    EXPECT_EQ(*second, 42);
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(HostTypesTest, ErrorPayloadRemainsRootedAfterCallbackAndInstanceAreReleased) {
  runtime::Isolate isolate(1);
  Persistent<RetainedCallback> saved;
  const CallbackSink sink(saved);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Keep>("keep")));
  {
    const auto compiled = test::CompileCleanSource(R"(
external func keep(callback: () => int): void;
class Failure { value: int; }
func main(): void { keep((): int => { throw Failure(value: 42); }); }
)",
                                                   isolate);
    ASSERT_TRUE(compiled) << compiled.error();
    ASSERT_TRUE(test::Execute(**compiled, "main").Succeeded());
  }
  std::optional<Error> error;
  {
    HandleScope scope(isolate);
    const auto callback = saved.ToLocal(scope);
    ASSERT_TRUE(callback);
    auto result = callback->Call(scope);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().status, ExecutionStatus::Thrown);
    error = std::move(result.error());
    saved.Reset();
  }
  isolate.heap().Collect();
  ASSERT_TRUE(error.has_value());
  EXPECT_FALSE(error->stack_trace.empty());
  EXPECT_FALSE(error->payload.IsEmpty());
  {
    HandleScope scope(isolate);
    const auto payload = error->payload.ToLocal(scope);
    ASSERT_TRUE(payload);
    error.reset();
    isolate.heap().Collect();
    const auto value = vela::internal::HandleAccess::Read(*payload);
    ASSERT_TRUE(runtime::Heap::IsThrowable(value));
    EXPECT_EQ(runtime::Heap::LoadField(*value.AsObject(), 0).AsInteger(), 42);
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(HostTypesTest, NestedNullableArrayRoundTripPreservesIdentityAcrossGc) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<EchoBuiltin<NestedIntegers>>("echo")));
  const auto compiled = test::CompileCleanSource(R"(
external func echo(values: int[]?[]): int[]?[];
func main(): int {
  const values: int[]?[] = [[40, 2], null];
  const returned = echo(values);
  const first = returned[0];
  if (first == null || returned[1] != null) { return -1; }
  first[0] += first[1];
  const original = values[0];
  return original == null ? -2 : original[0];
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  test::ExpectInteger(**compiled);
}

TEST(HostTypesTest, NullableScalarsRetainTheirTagsInBothDirections) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Increment>("increment")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Half>("half")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Flip>("flip")));
  const auto compiled = test::CompileCleanSource(R"(
external func increment(value: int?): int?;
external func half(value: double?): double?;
external func flip(value: bool?): bool?;
func main(): int {
  if (increment(null) != null || half(null) != null || flip(null) != null) { return -1; }
  if (half(84.0) != 42.0 || flip(false) != true || flip(true) != false) { return -2; }
  return increment(41) ?? -3;
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  test::ExpectInteger(**compiled);
}

TEST(HostTypesTest, DynamicLocalsKeepScalarsNullAndManagedIdentity) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<EchoBuiltin<Value>>("echo")));
  const auto compiled = test::CompileCleanSource(R"(
external func echo(value: dynamic): dynamic;
func main(): int {
  if (echo(null) != null || echo(42) != 42 || echo('text') != 'text') { return -1; }
  const values: dynamic = [40, 2];
  const returned = echo(values);
  returned[0] += returned[1];
  return values[0] as int;
}
)",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  test::ExpectInteger(**compiled);
}

TEST(HostTypesTest, RejectsDifferentNestedElementContractsBeforePublishingInstance) {
  runtime::Isolate isolate;
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<EchoBuiltin<NestedIntegers>>("echo")));
  const auto compiled = test::CompileCleanSource(
      "external func echo(values: double[]?[]): double[]?[]; func main(): void {}", isolate);
  ASSERT_FALSE(compiled);
  EXPECT_NE(compiled.error().find("native signature"), std::string_view::npos);
}

}  // namespace
}  // namespace vela::compiler
