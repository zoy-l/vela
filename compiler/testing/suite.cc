#include "compiler/testing/suite.h"
#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/execution_status.h"
#include "base/member.h"
#include "base/process_exit.h"
#include "compiler/testing/report.h"
#include "compiler/testing/result.h"
#include "compiler/testing/settings.h"
#include "runtime/async/execution_context.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/rooted_value.h"
#include "runtime/isolate/member_table.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/strings/string.h"
#include "runtime/testing/test_bridge.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"

namespace vela::testing {
namespace {

constexpr std::size_t kDifferenceNodeBudget = 256;

bool IsSuiteMain(const runtime::NativeInstance& instance, std::uint32_t index) {
  const auto& function = instance.image().functions.values().subspan(index).front();
  if ((function.flags & runtime::kNativeTypedFunction) == 0 || function.parameter_count != 0) {
    return false;
  }
  const auto& result = instance.call_signature(index).result;
  if ((function.flags & runtime::kNativeAsyncFunction) == 0) {
    return !result;
  }
  if (!result) {
    return false;
  }
  const auto& type = instance.layouts().types.at(*result);
  return type.object_kind == runtime::ObjectKind::Future && type.signature != nullptr &&
         !type.signature->result;
}

std::optional<Difference> CopyDifference(const runtime::MemberTable& layouts, runtime::Value value) {
  if (value.IsNull()) {
    return std::nullopt;
  }
  Difference result;
  std::vector<std::pair<runtime::Value, Difference*>> pending{{value, &result}};
  absl::flat_hash_set<const runtime::HeapObject*> visited;
  std::size_t remaining = kDifferenceNodeBudget - 1;
  while (!pending.empty()) {
    const auto [source, target] = pending.back();
    pending.pop_back();
    const auto path = layouts.ReadPublicField(source, "path");
    const auto actual = layouts.ReadPublicField(source, "actual");
    const auto children = layouts.ReadPublicField(source, "children");
    if (!path.IsString() || !actual.IsString() || !children.IsObject()) {
      target->actual = "<invalid difference>";
      continue;
    }
    target->path = path.AsString()->value();
    target->actual = actual.AsString()->value();
    if (!visited.insert(source.AsObject()).second) {
      target->actual = "<difference already shown>";
      continue;
    }
    const auto length = runtime::Heap::ArrayLength(*children.AsObject());
    if (!length) {
      target->actual = "<invalid difference children>";
      continue;
    }
    const auto count = std::min(static_cast<std::size_t>(*length), remaining);
    remaining -= count;
    target->children.resize(count + (count < *length ? 1U : 0U));
    for (std::size_t index = 0; index < count; ++index) {
      pending.emplace_back(
          *runtime::Heap::GetElement(*children.AsObject(), static_cast<std::uint32_t>(index)),
          &target->children.at(index));
    }
    if (count < *length) {
      target->children.back().actual = "<difference truncated>";
    }
  }
  return result;
}

std::expected<std::vector<std::string>, std::string> ReadTags(runtime::Value value) {
  if (value.IsNull()) {
    return std::vector<std::string>();
  }
  if (value.IsString()) {
    return std::vector<std::string>{std::string(value.AsString()->value())};
  }
  if (!value.IsObject()) {
    return std::unexpected("tags must be a string or string array");
  }
  const auto length = runtime::Heap::ArrayLength(*value.AsObject());
  if (!length) {
    return std::unexpected("tags must be a string or string array");
  }
  std::vector<std::string> tags;
  tags.reserve(*length);
  for (std::uint32_t index = 0; index < *length; ++index) {
    const auto element = *runtime::Heap::GetElement(*value.AsObject(), index);
    if (!element.IsString()) {
      return std::unexpected("tags array elements must be strings");
    }
    tags.emplace_back(element.AsString()->value());
  }
  return tags;
}

std::expected<std::optional<Skip>, std::string> ReadSkip(runtime::Value value) {
  if (value.IsNull()) {
    return std::nullopt;
  }
  if (value.IsBoolean()) {
    return Skip{.enabled = value.AsBoolean(), .reason = value.AsBoolean() ? "skipped" : ""};
  }
  if (value.IsString()) {
    return Skip{.enabled = true, .reason = std::string(value.AsString()->value())};
  }
  return std::unexpected("skip must be false, true, or a string");
}

constexpr std::size_t kSetUp = 0;
constexpr std::size_t kTearDown = 1;
constexpr std::size_t kSetUpAll = 2;
constexpr std::size_t kTearDownAll = 3;

}  // namespace

Suite::Suite(runtime::NativeInstance& instance,
             SuiteOptions options,
             std::ostream& diagnostics)
    : isolate_(&instance.isolate()),
      instance_(&instance),
      options_(std::move(options)),
      process_(*isolate_, diagnostics, [this](const auto& future, auto value) {
        Unhandled(future, value);
      }) {
  assert(isolate_->test_bridge() == nullptr);
  isolate_->SetTestBridge(this);
  scopes_.emplace(0, &root_);
  phases_.emplace(0, TestPhase::Registration);
  for (const auto& tag : options_.settings.tags) {
    for (const auto name : tag.selector.Tags()) {
      declared_tags_.emplace(name);
    }
  }
}

Suite::~Suite() {
  isolate_->scheduler().Shutdown();
  isolate_->SetTestBridge(nullptr);
}

Suite::Callback Suite::Capture(const runtime::TestRegistration& registration) {
  return {
      .id = next_id_++,
      .value = runtime::RootedValue(*isolate_, registration.callback),
      .trace = registration.trace,
  };
}

std::expected<void, std::string> Suite::ReadMetadata(
    Node& node,
    const runtime::TestRegistration& registration) {
  auto tags = ReadTags(registration.tags);
  if (!tags) {
    return std::unexpected(tags.error());
  }
  node.tags = std::move(*tags);
  auto skip = ReadSkip(registration.skip);
  if (!skip) {
    return std::unexpected(skip.error());
  }
  node.skip = std::move(*skip);
  switch (registration.timeout_mode) {
    case -1:
      break;
    case 0:
      node.timeout = Timeout{.duration = std::chrono::milliseconds(registration.milliseconds)};
      break;
    case 1:
      node.timeout = Timeout{.kind = TimeoutKind::Factor, .factor = registration.multiplier};
      break;
    case 2:
      node.timeout = Timeout{.kind = TimeoutKind::None};
      break;
    default:
      return std::unexpected("invalid SDK timeout mode");
  }
  return {};
}

std::expected<void, std::string> Suite::RegisterNode(
    const runtime::TestRegistration& registration) {
  auto node = std::make_unique<Node>();
  node->id = next_id_++;
  node->parent = registration_scope_;
  node->description = registration.description;
  node->trace = registration.trace;
  const auto metadata = ReadMetadata(*node, registration);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  const bool group = registration.operation == runtime::TestOperation::EnterGroup;
  node->kind = group ? NodeKind::Group : NodeKind::Test;
  if (!group) {
    node->body = Capture(registration);
  }
  auto* registered = node.get();
  scopes_.emplace(registered->id, registered);
  registration_scope_->children.push_back(std::move(node));
  if (group) {
    registration_scope_ = registered;
  }
  return {};
}

std::expected<void, std::string> Suite::RegisterHook(
    const runtime::TestRegistration& registration) {
  auto callback = Capture(registration);
  if (registration.operation == runtime::TestOperation::AddTearDown) {
    const auto context = isolate_->scheduler().context();
    if (!context || state_ != State::Running) {
      return std::unexpected("addTearDown requires an executing test or setup scope");
    }
    auto* scope = scopes_.at(context->scope);
    if (scope->closed) {
      return std::unexpected("the cleanup scope has already closed");
    }
    scope->cleanup.push_back(std::move(callback));
    return {};
  }
  const auto index = static_cast<std::size_t>(registration.operation) -
                     static_cast<std::size_t>(runtime::TestOperation::SetUp);
  registration_scope_->hooks.at(index).push_back(std::move(callback));
  return {};
}

std::expected<void, std::string> Suite::Register(const runtime::TestRegistration& registration) {
  const auto operation = registration.operation;
  if (operation == runtime::TestOperation::AddTearDown) {
    return RegisterHook(registration);
  }
  if (state_ != State::Registering) {
    return std::unexpected("test registration is closed");
  }
  if (operation == runtime::TestOperation::Test ||
      operation == runtime::TestOperation::EnterGroup) {
    return RegisterNode(registration);
  }
  if (operation == runtime::TestOperation::ExitGroup) {
    registration_scope_ = registration_scope_->parent;
    return {};
  }
  return RegisterHook(registration);
}

std::expected<void, std::string> Suite::Configure(Node& node, TestResult& result) {
  auto timeout = options_.settings.timeout.Apply(kDefaultTimeout);
  if (!timeout) {
    return std::unexpected(timeout.error());
  }
  std::optional<Skip> skip;
  for (const auto& tag : options_.settings.tags) {
    if (!tag.selector.Matches(result.tags)) {
      continue;
    }
    if (tag.timeout) {
      timeout = tag.timeout->Apply(*timeout);
      if (!timeout) {
        return std::unexpected(timeout.error());
      }
    }
    if (tag.skip) {
      skip = tag.skip;
    }
  }
  std::vector<const Node*> scopes;
  for (const auto* scope = &node; scope != nullptr; scope = scope->parent) {
    scopes.push_back(scope);
  }
  for (const auto* scope : scopes | std::views::reverse) {
    if (scope->timeout) {
      timeout = scope->timeout->Apply(*timeout);
      if (!timeout) {
        return std::unexpected(timeout.error());
      }
    }
    if (scope->skip) {
      skip = scope->skip;
    }
  }
  node.effective_timeout = *timeout;
  if (skip && skip->enabled) {
    result.status = TestStatus::Skipped;
    result.skip_reason = skip->reason;
  }
  return {};
}

std::expected<bool, std::string> Suite::IsSelected(const TestResult& result) const {
  const auto& settings = options_.settings;
  if (settings.include_tags && !settings.include_tags->Matches(result.tags)) {
    return false;
  }
  if (settings.exclude_tags && settings.exclude_tags->Matches(result.tags)) {
    return false;
  }
  return options_.select ? options_.select(result) : std::expected<bool, std::string>(true);
}

std::expected<void, std::string> Suite::Select(Node& node,
                                               const std::string& prefix,
                                               const std::vector<std::string>& inherited_tags) {
  const auto name = prefix.empty() ? node.description : prefix + " " + node.description;
  auto tags = inherited_tags;
  tags.insert(tags.end(), node.tags.begin(), node.tags.end());
  for (const auto& tag : node.tags) {
    if (!declared_tags_.contains(tag) && warned_tags_.insert(tag).second) {
      ReportEvent warning(ReportEventType::Warning);
      warning.scope = node.id;
      warning.text = "undeclared tag '" + tag + "'";
      warning.trace = node.trace;
      Emit(std::move(warning));
    }
  }
  TestResult result{
      .id = node.id,
      .name = name,
      .tags = tags,
      .skip_reason = {},
      .registration_trace = node.trace,
      .errors = {},
  };
  const auto configured = Configure(node, result);
  if (!configured) {
    return configured;
  }
  if (node.kind == NodeKind::Test) {
    const auto selected = IsSelected(result);
    if (!selected) {
      result_.exit_code = kRuntimeErrorExitCode;
      TestError error{
          .phase = TestPhase::Registration,
          .category = "FilterError",
          .message = selected.error(),
          .trace = node.trace,
          .difference = std::nullopt,
      };
      result_.errors.push_back(error);
      ReportEvent event(ReportEventType::Error);
      event.error = std::move(error);
      Emit(std::move(event));
      return std::unexpected(selected.error());
    }
    if (!*selected) {
      return {};
    }
    node.result = result_.tests.size();
    node.runnable = result.status == TestStatus::Skipped ? 0 : 1;
    result_.tests.push_back(std::move(result));
    EmitTest(node, ReportEventType::Test);
    return {};
  }
  ReportEvent event(ReportEventType::Group);
  event.scope = node.id;
  if (node.parent != nullptr) {
    event.parent = node.parent->id;
  }
  event.name = name;
  event.tags = tags;
  event.skip_reason = result.skip_reason;
  event.trace = node.trace;
  Emit(std::move(event));
  for (auto& child : node.children) {
    const auto selected = Select(*child, name, tags);
    if (!selected) {
      return selected;
    }
    node.runnable += child->runnable;
  }
  return {};
}

TestError Suite::Describe(const runtime::ExecutionResult& result, TestPhase phase) const {
  TestError error{
      .phase = phase,
      .category = std::string(ExecutionStatusCode(result.status)),
      .message = result.message,
      .trace = result.stack_trace,
      .difference = std::nullopt,
  };
  if (result.value.IsNull() && result.message.starts_with("TestTimeout:")) {
    error.category = "TestTimeout";
  }
  if (error.message.empty()) {
    std::ostringstream output;
    runtime::WriteValue(output, result.value);
    error.message = output.str();
  }
  const auto& layouts = instance_->members();
  error.difference = CopyDifference(layouts, layouts.ReadPublicField(result.value, "difference"));
  if (error.difference) {
    error.category = "TestFailure";
  }
  const auto* message = layouts.Find(result.value, "message");
  if (message != nullptr && message->kind == MemberKind::Field) {
    const auto* value = runtime::Heap::GetField(*result.value.AsObject(), message->target);
    if (value != nullptr && value->IsString()) {
      error.message = value->AsString()->value();
    }
  }
  DescribeMockFailure(result.value, error);
  return error;
}

void Suite::Invoke(const Callback& callback,
                   TestPhase phase,
                   Node& scope,
                   std::vector<TestError>& errors) {
  const auto invocation = next_invocation_++;
  phases_.emplace(invocation, phase);
  if (phase != TestPhase::Body) {
    hook_invocations_.emplace(invocation, callback.id);
  }
  const runtime::ExecutionContextScope context(isolate_->scheduler(),
                                               runtime::ExecutionContext{
                                                   .session = options_.session,
                                                   .suite = options_.suite,
                                                   .scope = scope.id,
                                                   .invocation = invocation,
                                               });
  scope.heartbeat = runtime::FutureScheduler::Clock::now();
  const bool cleanup = IsCleanupPhase(phase);
  if (!cleanup && options_.is_cancelled && options_.is_cancelled()) {
    scope.cancelled = true;
    return;
  }
  auto result = Await(runtime::ExecuteNativeCallable(*instance_, callback.value.value()), scope.effective_timeout,
                      scope, phase);
  if (result.stack_trace.empty() && !result.Succeeded()) {
    result.stack_trace = callback.trace;
  }
  const runtime::RootRange result_root(isolate_->heap(), &result.value, 1,
                                       runtime::RootOwner::Runtime);
  if (!result.Succeeded()) {
    RecordError(result, phase, errors);
  }
  while (isolate_->scheduler().PumpOne(runtime::FuturePumpMode::ReadyWork)) {
  }
  if (phase != TestPhase::SetUp) {
    WaitExpectations(scope, phase, errors);
  }
  if (!cleanup && options_.is_cancelled && options_.is_cancelled()) {
    scope.cancelled = true;
  }
}

void Suite::RunHooks(Node& group,
                     std::size_t index,
                     TestPhase phase,
                     Node& target,
                     std::vector<TestError>& errors) {
  const auto& hooks = group.hooks.at(index);
  if (index == kTearDown || index == kTearDownAll) {
    for (const auto& hook : hooks | std::views::reverse) {
      Invoke(hook, phase, target, errors);
    }
  } else {
    for (const auto& hook : hooks) {
      const auto previous = errors.size();
      Invoke(hook, phase, target, errors);
      if (errors.size() != previous || target.cancelled) {
        break;
      }
    }
  }
}

void Suite::RunCleanup(Node& scope, std::vector<TestError>& errors) {
  while (!scope.cleanup.empty()) {
    auto callback = std::move(scope.cleanup.back());
    scope.cleanup.pop_back();
    Invoke(callback, TestPhase::DynamicTearDown, scope, errors);
  }
}

void Suite::RunTest(Node& test,
                    const std::vector<Node*>& ancestry,
                    const std::vector<TestError>& blocked) {
  if (!test.result) {
    return;
  }
  auto& result = result_.tests.at(*test.result);
  EmitTest(test, ReportEventType::TestStart);
  if (result.status == TestStatus::Skipped) {
    EmitTest(test, ReportEventType::TestDone);
    return;
  }
  if (options_.is_cancelled && options_.is_cancelled()) {
    result.status = TestStatus::Cancelled;
    EmitTest(test, ReportEventType::TestDone);
    return;
  }
  result.status = TestStatus::Passed;
  auto& errors = result.errors;
  if (!blocked.empty()) {
    errors = blocked;
    result.status = TestStatus::Failed;
    EmitTest(test, ReportEventType::TestDone);
    return;
  }
  std::size_t entered = 0;
  for (auto* scope : ancestry) {
    ++entered;
    RunHooks(*scope, kSetUp, TestPhase::SetUp, test, errors);
    if (!errors.empty() || test.cancelled) {
      break;
    }
  }
  if (errors.empty() && !test.cancelled) {
    Invoke(*test.body, TestPhase::Body, test, errors);
  }
  RunCleanup(test, errors);
  while (entered != 0) {
    RunHooks(*ancestry.at(--entered), kTearDown, TestPhase::TearDown, test, errors);
  }
  // SDK 范围收尾必须晚于全部用户清理，通过原生调用执行并推进错误检查点。
  while (!test.finalizers.empty()) {
    auto callback = std::move(test.finalizers.back());
    test.finalizers.pop_back();
    Invoke(callback, TestPhase::TearDown, test, errors);
  }
  if (!errors.empty()) {
    result.status = TestStatus::Failed;
  }
  if (test.cancelled) {
    result.status = TestStatus::Cancelled;
  }
  test.closed = true;
  EmitTest(test, ReportEventType::TestDone);
}

void Suite::RunGroup(Node& group,
                     std::vector<Node*>& ancestry,
                     const std::vector<TestError>& blocked) {
  if (group.runnable == 0) {
    for (auto& child : group.children) {
      if (child->kind == NodeKind::Group) {
        RunGroup(*child, ancestry, blocked);
      } else {
        RunTest(*child, ancestry, blocked);
      }
    }
    return;
  }
  ancestry.push_back(&group);
  if (blocked.empty()) {
    RunHooks(group, kSetUpAll, TestPhase::SetUpAll, group, group.errors);
  }
  auto failures = blocked;
  failures.insert(failures.end(), group.errors.begin(), group.errors.end());
  for (auto& child : group.children) {
    if (child->kind == NodeKind::Group) {
      RunGroup(*child, ancestry, failures);
    } else {
      RunTest(*child, ancestry, failures);
    }
  }
  if (blocked.empty()) {
    RunCleanup(group, group.errors);
    RunHooks(group, kTearDownAll, TestPhase::TearDownAll, group, group.errors);
    result_.errors.insert(result_.errors.end(), group.errors.begin(), group.errors.end());
  }
  ancestry.pop_back();
  group.closed = true;
}

SuiteResult Suite::Run() {
  const runtime::ExecutionContextScope context(isolate_->scheduler(),
                                               runtime::ExecutionContext{
                                                   .session = options_.session,
                                                   .suite = options_.suite,
                                                   .scope = 0,
                                                   .invocation = 0,
                                               });
  root_.heartbeat = runtime::FutureScheduler::Clock::now();
  if (options_.is_cancelled && options_.is_cancelled()) {
    RecordError(WaitFailure(WaitStatus::Cancelled), TestPhase::Registration, result_.errors);
    return Finish();
  }
  const auto main = instance_->FindFunction("main");
  if (!main || !IsSuiteMain(*instance_, *main)) {
    RegistrationError("RegistrationError",
                      "test suite requires main(): void or main(): Future<void>");
    return Finish();
  }
  auto load_timeout = options_.settings.suite_load_timeout.Apply(kDefaultTimeout);
  if (options_.load_deadline) {
    load_timeout = std::max(std::chrono::milliseconds(0),
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                *options_.load_deadline - root_.heartbeat));
  }
  const auto registered =
      Await(runtime::ExecuteNativeFunction(*instance_, *main), *load_timeout, root_, TestPhase::Registration);
  const runtime::RootRange registration_root(isolate_->heap(), &registered.value, 1,
                                             runtime::RootOwner::Runtime);
  if (!registered.Succeeded()) {
    RecordError(registered, TestPhase::Registration, result_.errors);
    return Finish();
  }
  while (isolate_->scheduler().PumpOne(runtime::FuturePumpMode::ReadyWork)) {
  }
  if (options_.load_deadline && runtime::FutureScheduler::Clock::now() >= *options_.load_deadline) {
    RecordError(WaitFailure(WaitStatus::TimedOut), TestPhase::Registration, result_.errors);
  }
  if (!result_.errors.empty()) {
    return Finish();
  }
  state_ = State::Running;
  const auto selected = Select(root_, {}, {});
  if (!selected) {
    if (result_.errors.empty()) {
      RegistrationError("RegistrationError", selected.error());
    }
  } else if (result_.tests.empty()) {
    RegistrationError("EmptyResult", "no tests selected");
  } else if (!options_.list) {
    std::vector<Node*> ancestry;
    RunGroup(root_, ancestry, {});
  }
  return Finish();
}

void Suite::Unhandled(const runtime::Future& future, runtime::Value value) {
  (void)value;
  const auto& completion = future.result();
  const auto& origin = completion.diagnostic.origin.context;
  if (!origin || origin->session != options_.session || origin->suite != options_.suite) {
    DeliverError(completion, 0, TestPhase::Registration);
    return;
  }
  const auto phase = phases_.at(origin->invocation);
  DeliverError(completion, origin->scope, phase);
}

void Suite::RecordError(const runtime::ExecutionResult& failure,
                        TestPhase phase,
                        std::vector<TestError>& errors) {
  const auto identity = failure.origin.identity;
  if (failure.status == ExecutionStatus::OutOfMemory ||
      failure.status == ExecutionStatus::InvalidIsolate ||
      failure.status == ExecutionStatus::InvalidEntry) {
    result_.exit_code = kRuntimeErrorExitCode;
  }
  if (identity != 0 && !reported_errors_.insert(identity).second) {
    return;
  }
  errors.push_back(Describe(failure, phase));
  ReportEvent event(ReportEventType::Error);
  event.error = errors.back();
  const auto context =
      failure.origin.captured ? failure.origin.context : isolate_->scheduler().context();
  if (context) {
    event.scope = context->scope;
    if (const auto hook = hook_invocations_.find(context->invocation);
        hook != hook_invocations_.end()) {
      event.hook = hook->second;
    }
    if (scopes_.at(context->scope)->kind == NodeKind::Test) {
      event.test = context->scope;
    }
  }
  Emit(std::move(event));
}

}  // namespace vela::testing
