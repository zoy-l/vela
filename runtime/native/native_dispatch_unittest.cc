#include "runtime/native/native_dispatch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <gtest/gtest.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "base/source_location.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_image_test_support.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

using test::Table;
using test::Text;

constexpr DeclarationId kModule{1};
constexpr DeclarationId kAnswer{10};
constexpr DeclarationId kCaller{20};
constexpr DeclarationId kReplacement{30};
constexpr std::int64_t kOldValue = 40;
constexpr std::int64_t kNewValue = 42;
constexpr std::int64_t kOptionalValue = 2;
constexpr std::uint32_t kSourceLineStride = 10;
constexpr std::uint32_t kOptionalAndRestArgumentCount = 6;
constexpr std::uint32_t kBoundArgumentCount = 4;

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

void ExpectArgument(std::span<const NativeValue> arguments, std::size_t index, Value expected) {
  const auto& argument = arguments[index];
  const auto encoded = EncodeNativeValue(expected);
  EXPECT_EQ(argument.kind, encoded.kind) << "argument " << index;
  EXPECT_EQ(argument.payload, encoded.payload) << "argument " << index;
}

std::uint8_t InspectOptionalAndRest(NativeExecution* /*unused*/,
                                    const NativeValue* arguments,
                                    std::uint64_t count,
                                    NativeValue* output) noexcept {
  if (count != kOptionalAndRestArgumentCount) {
    return static_cast<std::uint8_t>(ExecutionStatus::TypeError);
  }
  const auto values = std::span(arguments, count);
  ExpectArgument(values, 0, Value::Integer(kOldValue));
  const bool optional_present = !DecodeNativeValue(values[1]).IsNull();
  if (optional_present) {
    ExpectArgument(values, 1, Value::Integer(kOptionalValue));
  }
  const auto* rest = DecodeNativeValue(values[2]).AsObjectOrNull();
  if (rest == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::TypeError);
  }
  EXPECT_EQ(Heap::ArrayLength(*rest), 0U);
  ExpectArgument(values, 3, Value::Boolean(true));
  ExpectArgument(values, 4, Value::Boolean(optional_present));
  ExpectArgument(values, values.size() - 1, Value::Boolean(false));
  *output = EncodeNativeValue(Value::Integer(optional_present ? 1 : 0));
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

std::uint8_t InspectBoundPresence(NativeExecution* /*unused*/,
                                  const NativeValue* arguments,
                                  std::uint64_t count,
                                  NativeValue* output) noexcept {
  if (count != kBoundArgumentCount) {
    return static_cast<std::uint8_t>(ExecutionStatus::TypeError);
  }
  const auto values = std::span(arguments, count);
  ExpectArgument(values, 1, Value::Null());
  ExpectArgument(values, 2, Value::Boolean(true));
  ExpectArgument(values, 3, Value::Boolean(false));
  *output = values.front();
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

std::uint8_t CallLatest(NativeExecution* execution,
                        const NativeValue* /*unused*/,
                        std::uint64_t /*unused*/,
                        NativeValue* output) noexcept {
  NativeTraceFrame frame;
  execution->MaterializeFrame(frame, 1);
  const auto result = vela_native_v3_call_declaration(execution, kAnswer.value, nullptr, 0, output);
  execution->Exit();
  return result;
}

std::uint8_t Fail(NativeExecution* execution,
                  const NativeValue* /*unused*/,
                  std::uint64_t /*unused*/,
                  NativeValue* /*unused*/) noexcept {
  NativeTraceFrame frame;
  execution->MaterializeFrame(frame, 0);
  const auto result = execution->HandleFailure(ExecutionStatus::DivisionByZero);
  execution->Exit();
  return static_cast<std::uint8_t>(result);
}

struct VersionedImage final {
  VersionedImage(std::uint64_t revision, NativeEntry answer) {
    image.revision = revision;
    image.native_mode = static_cast<std::uint32_t>(NativeProgramMode::Reloadable);
    types.front().kind = static_cast<std::uint32_t>(layout::Type::Kind::Int);
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
        .generic_signature = {},
        .definition = {},
    };
    declarations.at(1) = {
        .identity = kAnswer,
        .owner = kModule,
        .kind = static_cast<std::uint32_t>(DeclarationKind::Function),
        .name = Text("answer"),
        .type_parameters = {},
        .enum_members = {},
        .generic_signature = {},
        .definition = {},
    };
    declarations.back() = {
        .identity = kCaller,
        .owner = kModule,
        .kind = static_cast<std::uint32_t>(DeclarationKind::Function),
        .name = Text("caller"),
        .type_parameters = {},
        .enum_members = {},
        .generic_signature = {},
        .definition = {},
    };
    image.functions = Table(functions);
    image.declarations = Table(declarations);
    image.types = Table(types);
    auto& callable_class = classes.front();
    callable_class.name = Text("answer value");
    callable_class.callable_function = 0;
    callable_class.callable_mode = static_cast<std::uint32_t>(CallableMode::Plain);
    callable_class.callable_type = 0;
    signatures.front().name = Text("() => int");
    signatures.front().result = types.front();
    signatures.front().has_result = 1;
    image.signatures = Table(signatures);
    image.classes = Table(classes);
    image.source_paths = Table(paths);
    image.source_texts = Table(source);
  }
  NativeImage image = test::EmptyImage();
  std::array<NativeFunctionDescription, 2> functions;
  std::array<NativeDeclarationDescription, 3> declarations;
  std::array<NativeTypeDescription, 1> types;
  std::array<NativeClassDescription, 1> classes;
  std::array<NativeSignatureDescription, 1> signatures;
  std::array<SourceLocation, 1> locations;
  std::array<NativeText, 1> paths;
  std::array<NativeText, 1> source;
};

std::shared_ptr<const NativeImage> Image(const std::shared_ptr<VersionedImage>& owner) {
  return {owner, &owner->image};
}

std::expected<NativePublicationTimings, NativePublicationError> PublishVersion(
    NativeDispatch& dispatch,
    std::uint64_t base_revision,
    const std::shared_ptr<VersionedImage>& owner) {
  auto prepared = dispatch.Prepare(base_revision, Image(owner));
  if (!prepared) {
    return std::unexpected(prepared.error());
  }
  return dispatch.Publish(std::move(*prepared));
}

TEST(NativeDispatchTest, ClosedCapturedEntryOwnsItsProgramAndBlocksStopUntilReleased) {
  Isolate isolate(1);
  auto owner = std::make_shared<VersionedImage>(1, OldAnswer);
  owner->image.native_mode = static_cast<std::uint32_t>(NativeProgramMode::Closed);
  const std::weak_ptr<VersionedImage> lifetime = owner;
  auto created = NativeInstance::Instantiate(isolate, Image(owner));
  ASSERT_TRUE(created);
  auto bound = NativeVersionEntry::Bind(**created, 0);
  ASSERT_TRUE(bound);
  std::optional<NativeVersionEntry> captured = std::move(*bound);
  bound = std::unexpected(ExecutionStatus::InvalidEntry);
  created->reset();
  owner.reset();
  isolate.heap().Collect();
  ASSERT_FALSE(lifetime.expired());
  const auto result = captured->Invoke({});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), kOldValue);
  const auto refused = isolate.TryStop();
  ASSERT_FALSE(refused);
  ASSERT_EQ(refused.error().size(), 1U);
  EXPECT_EQ(refused.error().front().kind, ExternalResourceKind::NativeHandle);
  captured.reset();
  isolate.heap().Collect();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_TRUE(isolate.TryStop());
}

TEST(NativeDispatchTest, NamedEntryFollowsReloadAndCapturedEntriesKeepTheirOriginalCodeAlive) {
  Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  auto first = std::make_shared<VersionedImage>(1, OldAnswer);
  const std::weak_ptr<VersionedImage> old_lifetime = first;
  ASSERT_TRUE(PublishVersion(dispatch, 0, first));
  const auto entry = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->Invoke({})->AsInteger(), kOldValue);
  {
    const auto captured = entry->Capture();
    ASSERT_TRUE(captured);
    const auto caller = dispatch.FindEntry("main.vela", "caller");
    ASSERT_TRUE(caller);
    const auto old_caller = caller->Capture();
    ASSERT_TRUE(old_caller);
    auto second = std::make_shared<VersionedImage>(2, NewAnswer);
    std::swap(second->functions.front(), second->functions.back());
    ASSERT_TRUE(PublishVersion(dispatch, 1, second));
    first.reset();
    isolate.heap().Collect();
    EXPECT_FALSE(old_lifetime.expired());
    EXPECT_EQ(entry->Invoke({})->AsInteger(), kNewValue);
    EXPECT_EQ(captured->Invoke({})->AsInteger(), kOldValue);
    EXPECT_EQ(old_caller->Invoke({})->AsInteger(), kNewValue);
    EXPECT_EQ(captured->identity().revision, 1U);
  }
  isolate.heap().Collect();
  EXPECT_TRUE(old_lifetime.expired());
  EXPECT_EQ(entry->Invoke({})->AsInteger(), kNewValue);
  const auto blockers = isolate.ReloadBlockers();
  ASSERT_EQ(blockers.size(), 1U);
  EXPECT_EQ(blockers.front().kind, ExternalResourceKind::NativeNamedEntry);
  EXPECT_EQ(blockers.front().references, 1U);
  EXPECT_FALSE(isolate.TryStop());
}

TEST(NativeDispatchTest, UnpublishedAndClosedDispatchRejectAllLookupBoundaries) {
  Isolate isolate;
  auto& dispatch = isolate.native_dispatch();
  auto* nominal =
      isolate.CreateNominalType({.kind = NominalKind::Class, .interfaces = {}, .declaration = {}});
  const auto image = std::make_shared<VersionedImage>(1, OldAnswer);
  image->image.native_mode = static_cast<std::uint32_t>(NativeProgramMode::Closed);
  auto instance = NativeInstance::Instantiate(isolate, Image(image));
  ASSERT_TRUE(instance);
  NativeExecution execution(**instance);
  NativeGlobalCache cache;
  const auto expect_lookup_errors = [&](ExecutionStatus status) {
    const auto expected = std::unexpected(status);
    EXPECT_EQ(dispatch.FindEntry("main.vela", "answer"), expected);
    EXPECT_EQ(dispatch.Resolve(kAnswer), expected);
    EXPECT_EQ(dispatch.ResolveVirtual(*nominal, kAnswer), expected);
    EXPECT_EQ(dispatch.LoadGlobal(execution, kAnswer, cache), expected);
    EXPECT_EQ(dispatch.StoreGlobal(kAnswer, Value::Null(), cache), expected);
  };
  expect_lookup_errors(ExecutionStatus::InvalidEntry);
  dispatch.Close();
  expect_lookup_errors(ExecutionStatus::ExecutionStopped);
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
  ++isolate.native_control().state().host_borrows;
  const auto busy = dispatch.Publish(std::move(*prepared));
  ASSERT_FALSE(busy);
  EXPECT_EQ(busy.error().status, NativePublicationStatus::Busy);
  --isolate.native_control().state().host_borrows;
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
    EXPECT_EQ(blockers.front().kind, ExternalResourceKind::NativePreparedPublication);
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
  ASSERT_TRUE(PublishVersion(dispatch, 0, first));
  const auto entry = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(entry);
  const auto old_caller = dispatch.FindEntry("main.vela", "caller")->Capture();
  ASSERT_TRUE(old_caller);
  const auto second = std::make_shared<VersionedImage>(2, NewAnswer);
  second->functions.front().identity.declaration = kReplacement;
  second->declarations.at(1).identity = kReplacement;
  ASSERT_TRUE(PublishVersion(dispatch, 1, second));
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
  ASSERT_TRUE(PublishVersion(dispatch, 0, first));
  const auto old_target = dispatch.Resolve(kCaller);
  ASSERT_TRUE(old_target);
  auto old_instance = NativeInstance::Borrow(*old_target->state);
  NativeExecution execution(old_instance);
  const auto second = std::make_shared<VersionedImage>(2, Fail);
  ASSERT_TRUE(PublishVersion(dispatch, 1, second));
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
  ASSERT_TRUE(PublishVersion(dispatch, 0, first));
  const auto old_target = dispatch.Resolve(kAnswer);
  ASSERT_TRUE(old_target);
  auto old_instance = NativeInstance::Borrow(*old_target->state);
  auto& heap = isolate.heap();
  auto* closure = heap.AllocateInstance(*old_instance.layouts().classes.front());
  ASSERT_NE(closure, nullptr);
  heap.SetNativeEnvironment(*closure, *old_target->state, old_target->function);
  const auto value_storage = HostRoot::Create(isolate, Value::Object(closure));
  ASSERT_TRUE(value_storage);
  const auto& value = *value_storage;
  const auto second = std::make_shared<VersionedImage>(2, NewAnswer);
  std::swap(second->functions.front(), second->functions.back());
  ASSERT_TRUE(PublishVersion(dispatch, 1, second));
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
  const auto callback = callbacks.resolve(value.value());
  ASSERT_TRUE(callback);
  EXPECT_EQ(callback->arity, 0U);
  const auto result = callback->invoke({});
  ASSERT_EQ(result.state(), FutureState::Succeeded);
  EXPECT_EQ(result.result().value.AsInteger(), 40);
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

TEST(NativeDispatchTest, IndirectCallsCompleteDefaultsAndRestAndRejectWrongArity) {
  Isolate isolate(1);
  auto image = std::make_shared<VersionedImage>(1, InspectOptionalAndRest);
  image->image.native_mode = static_cast<std::uint32_t>(NativeProgramMode::Closed);
  const std::array types{
      NativeTypeDescription{.kind = static_cast<std::uint32_t>(layout::Type::Kind::Int)},
      NativeTypeDescription{.kind = static_cast<std::uint32_t>(layout::Type::Kind::Array)},
  };
  const std::array arrays{NativeArrayDescription{.name = Text("int[]"), .element = types[0]}};
  const std::array parameters{
      NativeCallParameter{.name = Text("required"), .type = 0},
      NativeCallParameter{
          .name = Text("optional"),
          .type = 0,
          .flags = kNativeOptionalParameter | kNativeDefaultParameter,
      },
      NativeCallParameter{.name = Text("rest"), .type = 1, .flags = kNativeRestParameter},
  };
  const std::array signature{
      NativeSignatureParameter{.type = types[0]},
      NativeSignatureParameter{.type = types[0], .flags = kNativeOptionalParameter},
      NativeSignatureParameter{.type = types[1], .flags = kNativeRestParameter},
  };
  image->image.types = Table(types);
  image->image.arrays = Table(arrays);
  image->signatures.front().parameters = Table(signature);
  auto& function = image->functions.front();
  function.parameters = Table(parameters);
  function.parameter_count = kOptionalAndRestArgumentCount;
  const auto instance = NativeInstance::Instantiate(isolate, Image(image));
  ASSERT_TRUE(instance);
  auto& heap = isolate.heap();
  auto* object = heap.AllocateInstance(*(*instance)->layouts().classes.front());
  ASSERT_NE(object, nullptr);
  heap.SetNativeEnvironment(*object, *(*instance)->state().front().AsObject(), 0);
  const auto callable = HostRoot::Create(isolate, Value::Object(object));
  ASSERT_TRUE(callable);
  NativeExecution execution(**instance);
  const auto encoded = EncodeNativeValue(callable->value());
  std::array<NativeValue, parameters.size() + 2> arguments{};
  arguments[1] = EncodeNativeValue(Value::Integer(kOldValue));
  NativeValue output;
  EXPECT_EQ(static_cast<ExecutionStatus>(
                vela_native_v1_call_indirect(&execution, &encoded, arguments.data(), 0, &output)),
            ExecutionStatus::TypeError);
  ASSERT_EQ(static_cast<ExecutionStatus>(
                vela_native_v1_call_indirect(&execution, &encoded, arguments.data(), 1, &output)),
            ExecutionStatus::Returned);
  EXPECT_EQ(DecodeNativeValue(output).AsInteger(), 0);
  arguments[2] = EncodeNativeValue(Value::Integer(kOptionalValue));
  ASSERT_EQ(static_cast<ExecutionStatus>(
                vela_native_v1_call_indirect(&execution, &encoded, arguments.data(), 2, &output)),
            ExecutionStatus::Returned);
  EXPECT_EQ(DecodeNativeValue(output).AsInteger(), 1);
  EXPECT_EQ(static_cast<ExecutionStatus>(
                vela_native_v1_call_indirect(&execution, &encoded, arguments.data(), 4, &output)),
            ExecutionStatus::TypeError);
  const std::array callback_arguments{Value::Integer(kOldValue)};
  const auto callback = InvokeNativeCallable(execution, callable->value(), callback_arguments);
  ASSERT_TRUE(callback);
  EXPECT_EQ(callback->AsInteger(), 0);
}

TEST(NativeDispatchTest, BoundReceiverCountsAsPresentWhenItIsASignatureParameter) {
  Isolate isolate(1);
  auto image = std::make_shared<VersionedImage>(1, InspectBoundPresence);
  image->image.native_mode = static_cast<std::uint32_t>(NativeProgramMode::Closed);
  image->classes.front().field_count = 1;
  image->classes.front().callable_mode = static_cast<std::uint32_t>(CallableMode::BoundMethod);
  const std::array parameters{
      NativeCallParameter{.name = Text("receiver"), .type = 0},
      NativeCallParameter{
          .name = Text("optional"),
          .type = 0,
          .flags = kNativeOptionalParameter | kNativeDefaultParameter,
      },
  };
  auto& function = image->functions.front();
  function.parameters = Table(parameters);
  function.parameter_count = kBoundArgumentCount;
  const auto instance = NativeInstance::Instantiate(isolate, Image(image));
  ASSERT_TRUE(instance);
  auto& heap = isolate.heap();
  const std::array fields{Value::Integer(kOldValue)};
  auto* object = heap.AllocateInstance(*(*instance)->layouts().classes.front(), fields);
  ASSERT_NE(object, nullptr);
  heap.SetNativeEnvironment(*object, *(*instance)->state().front().AsObject(), 0);
  const auto callable = HostRoot::Create(isolate, Value::Object(object));
  ASSERT_TRUE(callable);
  NativeExecution execution(**instance);
  const auto encoded = EncodeNativeValue(callable->value());
  std::array<NativeValue, 1> arguments{};
  NativeValue output;
  ASSERT_EQ(static_cast<ExecutionStatus>(
                vela_native_v1_call_indirect(&execution, &encoded, arguments.data(), 0, &output)),
            ExecutionStatus::Returned);
  EXPECT_EQ(output.kind, NativeValueKind::Integer);
  EXPECT_EQ(DecodeNativeValue(output).AsInteger(), kOldValue);
  const auto callback = InvokeNativeCallable(execution, callable->value(), {});
  ASSERT_TRUE(callback);
  EXPECT_TRUE(callback->IsInteger());
  EXPECT_EQ(callback->AsInteger(), kOldValue);
}

}  // namespace
}  // namespace vela::runtime
