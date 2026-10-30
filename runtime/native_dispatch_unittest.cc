#include "runtime/native_dispatch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "runtime/external_resources.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_control.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/rooted_value.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

NativeText Text(std::string_view text) {
  return {.data = text.data(), .size = text.size()};
}

template <class T, std::size_t Size>
NativeTable<T> Table(const std::array<T, Size>& values) {
  return {.data = values.data(), .size = values.size()};
}

constexpr DeclarationId kModule{1};
constexpr DeclarationId kAnswer{10};
constexpr DeclarationId kCaller{20};
constexpr DeclarationId kReplacement{30};
constexpr std::int64_t kOldValue = 40;
constexpr std::int64_t kNewValue = 42;
constexpr std::uint32_t kSourceLineStride = 10;

std::uint8_t OldAnswer(NativeExecution* /*unused*/,
                       const NativeValue* /*unused*/,
                       std::uint64_t /*unused*/,
                       NativeValue* output) noexcept {
  *output = EncodeNativeValue(Value::Integer(kOldValue));
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

std::uint8_t NewAnswer(NativeExecution* /*unused*/,
                       const NativeValue* /*unused*/,
                       std::uint64_t /*unused*/,
                       NativeValue* output) noexcept {
  *output = EncodeNativeValue(Value::Integer(kNewValue));
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

std::uint8_t CallLatest(NativeExecution* execution,
                        const NativeValue* /*unused*/,
                        std::uint64_t /*unused*/,
                        NativeValue* output) noexcept {
  NativeTraceFrame frame;
  const auto status = execution->Enter(frame, 1);
  if (status != ExecutionStatus::Returned) {
    return static_cast<std::uint8_t>(status);
  }
  const auto result = vela_native_v3_call_declaration(execution, kAnswer.value, nullptr, 0, output);
  execution->Exit();
  return result;
}

std::uint8_t Fail(NativeExecution* execution,
                  const NativeValue* /*unused*/,
                  std::uint64_t /*unused*/,
                  NativeValue* /*unused*/) noexcept {
  NativeTraceFrame frame;
  const auto status = execution->Enter(frame, 0);
  if (status != ExecutionStatus::Returned) {
    return static_cast<std::uint8_t>(status);
  }
  const auto result = execution->HandleFailure(ExecutionStatus::DivisionByZero);
  execution->Exit();
  return static_cast<std::uint8_t>(result);
}

struct VersionedImage final {
  VersionedImage(std::uint64_t revision, NativeEntry answer) {
    image.abi_identity = Text(NativeRuntimeAbiIdentity());
    image.target = Text(NativeTargetIdentity());
    image.revision = revision;
    image.native_mode = static_cast<std::uint32_t>(NativeProgramMode::Reloadable);
    types.front().kind = static_cast<std::uint32_t>(layout::Type::Kind::Integer);
    locations.front().line = static_cast<std::uint32_t>(revision * kSourceLineStride);
    paths.front() = Text("main.vela");
    source.front() = Text(revision == 1 ? "old source" : "new source");
    auto& answer_function = functions.front();
    answer_function.name = Text("answer");
    answer_function.entry = answer;
    answer_function.identity.declaration = kAnswer;
    auto& caller_function = functions.back();
    caller_function.name = Text("caller");
    caller_function.entry = CallLatest;
    caller_function.identity.declaration = kCaller;
    for (auto& function : functions) {
      function.identity.revision = revision;
      function.result_type = 0;
      function.flags = kNativeExportedFunction | kNativeTypedFunction;
      function.locations = Table(locations);
    }
    declarations.front() = {
        .identity = kModule,
        .owner = {},
        .kind = static_cast<std::uint32_t>(DeclarationKind::Module),
        .name = Text("main.vela"),
        .type_parameters = {},
        .enum_members = {},
        .definition = {},
    };
    declarations.at(1) = {
        .identity = kAnswer,
        .owner = kModule,
        .kind = static_cast<std::uint32_t>(DeclarationKind::Function),
        .name = Text("answer"),
        .type_parameters = {},
        .enum_members = {},
        .definition = {},
    };
    declarations.back() = {
        .identity = kCaller,
        .owner = kModule,
        .kind = static_cast<std::uint32_t>(DeclarationKind::Function),
        .name = Text("caller"),
        .type_parameters = {},
        .enum_members = {},
        .definition = {},
    };
    image.functions = Table(functions);
    image.declarations = Table(declarations);
    image.types = Table(types);
    auto& callable_class = classes.front();
    callable_class.name = Text("answer value");
    callable_class.callable_function = 0;
    callable_class.callable_mode = static_cast<std::uint32_t>(CallableMode::Plain);
    image.classes = Table(classes);
    image.source_paths = Table(paths);
    image.source_texts = Table(source);
  }
  NativeImage image;
  std::array<NativeFunctionDescription, 2> functions;
  std::array<NativeDeclarationDescription, 3> declarations;
  std::array<NativeTypeDescription, 1> types;
  std::array<NativeClassDescription, 1> classes;
  std::array<SourceLocation, 1> locations;
  std::array<NativeText, 1> paths;
  std::array<NativeText, 1> source;
};

std::shared_ptr<const NativeImage> Image(const std::shared_ptr<VersionedImage>& owner) {
  return {owner, &owner->image};
}

TEST(NativeDispatchTest, NamedEntrySelectsCurrentVersionAndCapturedEntryKeepsItsOriginalCode) {
  Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  auto first = std::make_shared<VersionedImage>(1, OldAnswer);
  const std::weak_ptr<VersionedImage> old_lifetime = first;
  auto prepared = dispatch.Prepare(0, Image(first));
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  const auto entry = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->Invoke({})->AsInteger(), 40);
  std::optional<NativeVersionEntry> captured = *entry->Capture();
  auto caller = dispatch.FindEntry("main.vela", "caller");
  ASSERT_TRUE(caller);
  const auto old_caller = caller->Capture();
  ASSERT_TRUE(old_caller);
  auto second = std::make_shared<VersionedImage>(2, NewAnswer);
  std::swap(second->functions.front(), second->functions.back());
  auto next = dispatch.Prepare(1, Image(second));
  ASSERT_TRUE(next);
  ASSERT_TRUE(dispatch.Publish(std::move(*next)));
  first.reset();
  isolate.heap().Collect();
  EXPECT_FALSE(old_lifetime.expired());
  EXPECT_EQ(entry->Invoke({})->AsInteger(), 42);
  EXPECT_EQ(captured->Invoke({})->AsInteger(), 40);
  EXPECT_EQ(old_caller->Invoke({})->AsInteger(), 42);
  EXPECT_EQ(captured->identity().revision, 1U);
}

TEST(NativeDispatchTest, BusyPublicationKeepsItsCandidateUntilHostAndInitializationScopesExit) {
  Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto image = std::make_shared<VersionedImage>(1, OldAnswer);
  auto prepared = dispatch.Prepare(0, Image(image));
  ASSERT_TRUE(prepared);
  {
    const NativeInitializationScope initialization(isolate.native_control());
    const auto busy = dispatch.Publish(std::move(*prepared));
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().status, NativePublicationStatus::Busy);
    EXPECT_EQ(dispatch.revision(), 0U);
  }
  isolate.native_control().EnterHost();
  const auto busy = dispatch.Publish(std::move(*prepared));
  ASSERT_FALSE(busy);
  EXPECT_EQ(busy.error().status, NativePublicationStatus::Busy);
  isolate.native_control().ExitHost();
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  EXPECT_EQ(dispatch.revision(), 1U);
}

TEST(NativeDispatchTest, StopRejectsPreparedAndNewPublicationsWithoutAdvancingTheRevision) {
  Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  auto first = dispatch.Prepare(0, Image(std::make_shared<VersionedImage>(1, OldAnswer)));
  ASSERT_TRUE(first);
  ASSERT_TRUE(dispatch.Publish(std::move(*first)));
  const auto image = Image(std::make_shared<VersionedImage>(2, NewAnswer));
  auto next = dispatch.Prepare(1, image);
  ASSERT_TRUE(next);
  isolate.native_control().RequestStop();
  const auto rejected = dispatch.Publish(std::move(*next));
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, NativePublicationStatus::Stopped);
  EXPECT_EQ(dispatch.revision(), 1U);
  const auto refused = dispatch.Prepare(1, image);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().status, NativePublicationStatus::Stopped);
}

TEST(NativeDispatchTest, NamedEntryDoesNotKeepHistoricalCodeAlive) {
  Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  auto first = std::make_shared<VersionedImage>(1, OldAnswer);
  const std::weak_ptr<VersionedImage> lifetime = first;
  auto prepared = dispatch.Prepare(0, Image(first));
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  const auto named = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(named);
  auto version = std::optional<NativeVersionEntry>(*named->Capture());
  auto second = std::make_shared<VersionedImage>(2, NewAnswer);
  auto replacement = dispatch.Prepare(1, Image(second));
  ASSERT_TRUE(replacement);
  ASSERT_TRUE(dispatch.Publish(std::move(*replacement)));
  first.reset();
  isolate.heap().Collect();
  EXPECT_FALSE(lifetime.expired());
  version.reset();
  isolate.heap().Collect();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(named->Invoke({})->AsInteger(), 42);
  const auto blockers = isolate.ReloadBlockers();
  ASSERT_EQ(blockers.size(), 1U);
  EXPECT_EQ(blockers.front().kind, ExternalResourceKind::NativeEntry);
  EXPECT_EQ(blockers.front().references, 1U);
  EXPECT_FALSE(isolate.TryStop());
}

TEST(NativeDispatchTest, DroppedAndStaleCandidatesLeaveThePublishedProgramUntouched) {
  Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  auto first = std::make_shared<VersionedImage>(1, OldAnswer);
  {
    const auto discarded = dispatch.Prepare(0, Image(first));
    ASSERT_TRUE(discarded);
    EXPECT_EQ(dispatch.revision(), 0U);
  }
  {
    auto accepted = dispatch.Prepare(0, Image(first));
    auto stale = dispatch.Prepare(0, Image(first));
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(stale);
    ASSERT_TRUE(dispatch.Publish(std::move(*accepted)));
    const auto rejected = dispatch.Publish(std::move(*stale));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().status, NativePublicationStatus::Stale);
    EXPECT_EQ(dispatch.revision(), 1U);
    const auto blockers = isolate.ReloadBlockers();
    ASSERT_EQ(blockers.size(), 1U);
    EXPECT_EQ(blockers.front().kind, ExternalResourceKind::NativeCandidate);
    EXPECT_FALSE(isolate.TryStop());
  }
  EXPECT_TRUE(isolate.TryStop());
  const auto stopped = dispatch.Prepare(1, Image(first));
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().status, NativePublicationStatus::Stopped);
}

TEST(NativeDispatchTest, DeletedDeclarationNeverResolvesToAnotherFunctionAtTheSameIndex) {
  Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first = std::make_shared<VersionedImage>(1, OldAnswer);
  auto prepared = dispatch.Prepare(0, Image(first));
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  const auto entry = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(entry);
  const auto old_caller = dispatch.FindEntry("main.vela", "caller")->Capture();
  ASSERT_TRUE(old_caller);
  const auto second = std::make_shared<VersionedImage>(2, NewAnswer);
  second->functions.front().identity.declaration = kReplacement;
  second->declarations.at(1).identity = kReplacement;
  auto replacement = dispatch.Prepare(1, Image(second));
  ASSERT_TRUE(replacement);
  ASSERT_TRUE(dispatch.Publish(std::move(*replacement)));
  const auto deleted = entry->Invoke({});
  ASSERT_FALSE(deleted);
  EXPECT_EQ(deleted.error(), ExecutionStatus::TargetDeleted);
  const auto called = old_caller->Invoke({});
  ASSERT_FALSE(called);
  EXPECT_EQ(called.error(), ExecutionStatus::TargetDeleted);
}

TEST(NativeDispatchTest, ExceptionFramesResolveAgainstTheirOwnImmutableSourceVersions) {
  Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first = std::make_shared<VersionedImage>(1, OldAnswer);
  auto prepared = dispatch.Prepare(0, Image(first));
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  const auto old_target = dispatch.Resolve(kCaller);
  ASSERT_TRUE(old_target);
  auto old_instance = NativeInstance::Borrow(*old_target->state);
  NativeExecution execution(old_instance);
  const auto second = std::make_shared<VersionedImage>(2, Fail);
  auto replacement = dispatch.Prepare(1, Image(second));
  ASSERT_TRUE(replacement);
  ASSERT_TRUE(dispatch.Publish(std::move(*replacement)));
  NativeValue output;
  EXPECT_EQ(static_cast<ExecutionStatus>(CallLatest(&execution, nullptr, 0, &output)),
            ExecutionStatus::DivisionByZero);
  const auto diagnostic = execution.ExceptionDiagnostic();
  ASSERT_EQ(diagnostic.stack_trace.size(), 2U);
  EXPECT_EQ(diagnostic.stack_trace.front().function, "answer");
  EXPECT_EQ(diagnostic.stack_trace.front().identity.revision, 2U);
  EXPECT_EQ(diagnostic.stack_trace.front().line, 20U);
  EXPECT_EQ(diagnostic.stack_trace.at(1).function, "caller");
  EXPECT_EQ(diagnostic.stack_trace.at(1).identity.revision, 1U);
  EXPECT_EQ(diagnostic.stack_trace.at(1).line, 10U);
}

TEST(NativeDispatchTest, CallableValuesUseTheirProducingVersionAcrossCallsAndFutureCallbacks) {
  Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first = std::make_shared<VersionedImage>(1, OldAnswer);
  auto prepared = dispatch.Prepare(0, Image(first));
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  const auto old_target = dispatch.Resolve(kAnswer);
  ASSERT_TRUE(old_target);
  auto old_instance = NativeInstance::Borrow(*old_target->state);
  auto& heap = isolate.heap();
  auto* closure = heap.AllocateInstance(*old_instance.layouts().classes.front());
  ASSERT_NE(closure, nullptr);
  heap.SetNativeEnvironment(*closure, *old_target->state, old_target->function);
  const RootedValue value(isolate, Value::Object(closure));
  const auto second = std::make_shared<VersionedImage>(2, NewAnswer);
  std::swap(second->functions.front(), second->functions.back());
  auto replacement = dispatch.Prepare(1, Image(second));
  ASSERT_TRUE(replacement);
  ASSERT_TRUE(dispatch.Publish(std::move(*replacement)));
  const auto latest = dispatch.Resolve(kAnswer);
  ASSERT_TRUE(latest);
  auto instance = NativeInstance::Borrow(*latest->state);
  NativeExecution execution(instance);
  const auto callable = EncodeNativeValue(value.value());
  std::array<NativeValue, 1> arguments;
  NativeValue output;
  EXPECT_EQ(static_cast<ExecutionStatus>(
                vela_native_v1_call_indirect(&execution, &callable, arguments.data(), 0, &output)),
            ExecutionStatus::Returned);
  EXPECT_EQ(DecodeNativeValue(output).AsInteger(), 40);
  const auto callbacks = NativeFutureCallbacks(instance);
  const auto arity = callbacks.arity(value.value());
  ASSERT_TRUE(arity);
  EXPECT_EQ(*arity, 0U);
  const auto result = callbacks.invoke(value.value(), {});
  const auto completed_value = result.value();
  if (!completed_value) {
    FAIL() << "Future did not complete with a value";
  }
  EXPECT_EQ(completed_value->AsInteger(), 40);
  Isolate other;
  const auto without_session = NativeInstance::Instantiate(other, Image(second));
  ASSERT_FALSE(without_session);
  EXPECT_EQ(without_session.error().status, NativeInstantiationStatus::MissingSession);
  const auto closed = std::make_shared<VersionedImage>(1, OldAnswer);
  closed->image.native_mode = static_cast<std::uint32_t>(NativeProgramMode::Closed);
  const auto independent = NativeInstance::Instantiate(other, Image(closed));
  ASSERT_TRUE(independent);
  const auto foreign = ResolveNativeCallable(**independent, value.value());
  ASSERT_FALSE(foreign);
  EXPECT_EQ(foreign.error(), ExecutionStatus::InvalidIsolate);
}

}  // namespace
}  // namespace vela::runtime
