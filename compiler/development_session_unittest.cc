#include "compiler/development_session.h"

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "compiler/source_session.h"
#include "runtime/external_resources.h"
#include "runtime/future.h"
#include "runtime/isolate.h"
#include "runtime/native_context.h"
#include "runtime/native_function.h"
#include "runtime/rooted_value.h"
#include "runtime/value.h"

namespace vela::compiler {
namespace {

constexpr std::int64_t kAnswer = 42;
constexpr std::int64_t kOldAnswer = 40;

SourceUpdate Edit(const DevelopmentSession& session, std::string source) {
  return {
      .base_revision = session.snapshot()->revision,
      .changes = {{.path = "main.vela", .text = std::move(source)}},
      .added_roots = {},
  };
}

DevelopmentResult Apply(DevelopmentSession& session,
                        std::string source,
                        DevelopmentOperation operation = DevelopmentOperation::HotReload) {
  auto candidate = session.Prepare(Edit(session, std::move(source)), operation);
  if (!candidate) {
    return std::move(candidate.error());
  }
  auto submitted = session.Submit(std::move(*candidate));
  return submitted.status == DevelopmentStatus::WaitingForSafepoint ? session.Pump() : submitted;
}

void ExpectInteger(const std::expected<runtime::Value, ExecutionStatus>& result,
                   std::int64_t value) {
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), value);
}

void ExpectAcceptedSnapshot(const DevelopmentSession& session, const DevelopmentResult& result) {
  EXPECT_EQ(result.status, DevelopmentStatus::Accepted);
  EXPECT_EQ(session.snapshot()->revision, result.revision);
}

std::function<std::expected<void, ExecutionStatus>(DevelopmentSession&, const DevelopmentResult&)>
FailingSecondRefresh(std::uint32_t& notifications) {
  return [&notifications](DevelopmentSession& session,
                          const DevelopmentResult& result) -> std::expected<void, ExecutionStatus> {
    ++notifications;
    ExpectAcceptedSnapshot(session, result);
    if (result.revision == 2) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    return {};
  };
}

std::function<void()> QueueRequest(DevelopmentSession& session,
                                   std::string source,
                                   DevelopmentOperation operation) {
  return [&session, source = std::move(source), operation] {
    auto candidate = session.Prepare(Edit(session, source), operation);
    ASSERT_TRUE(candidate);
    EXPECT_EQ(session.Submit(std::move(*candidate)).status, DevelopmentStatus::WaitingForSafepoint);
  };
}

TEST(DevelopmentSessionTest, LoadDoesNotRunEntryAndHotReloadPreservesInitializedState) {
  auto created = DevelopmentSession::Create("main.vela");
  ASSERT_TRUE(created);
  auto& session = **created;
  auto result = Apply(session,
                      "let score = 10; func main(): void { score = 42; } "
                      "export func read(): int { return score; }",
                      DevelopmentOperation::Load);
  ASSERT_EQ(result.status, DevelopmentStatus::Accepted) << result.message;
  EXPECT_EQ(session.Invoke("main.vela", "read")->AsInteger(), 10);
  ASSERT_TRUE(session.RunEntry());
  result = Apply(session,
                 "let score = 100; func main(): void { score = 99; } "
                 "export func read(): int { return score + 1; }");
  ASSERT_EQ(result.status, DevelopmentStatus::Accepted) << result.message;
  EXPECT_EQ(session.Invoke("main.vela", "read")->AsInteger(), 43);
  EXPECT_EQ(session.snapshot()->revision, 2U);
  EXPECT_EQ(session.isolate().native_dispatch().revision(), 2U);
  EXPECT_EQ(result.changes.modules, std::vector<std::string>{"main.vela"});
}

TEST(DevelopmentSessionTest, GenericParameterChangesRequireReloadWithoutDamagingTheOldRun) {
  auto created = DevelopmentSession::Create("main.vela");
  ASSERT_TRUE(created);
  auto& session = **created;
  EXPECT_EQ(Apply(session,
                  "func value<T>(input: T): int { return 42; } "
                  "func main(): int { return value<int>(0); }")
                .status,
            DevelopmentStatus::Accepted);
  const std::string changed =
      "func value<T,U>(input: T): int { return 100; } "
      "func main(): int { return value<int,string>(0); }";
  const auto rejected = session.Prepare(Edit(session, changed), DevelopmentOperation::HotReload);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, DevelopmentStatus::RequiresReload);
  EXPECT_NE(rejected.error().declaration, DeclarationId{});
  constexpr std::int64_t kOriginalResult = 42;
  ExpectInteger(session.RunEntry(), kOriginalResult);
  EXPECT_EQ(Apply(session, changed, DevelopmentOperation::Reload).status,
            DevelopmentStatus::Accepted);
  constexpr std::int64_t kReloadedResult = 100;
  ExpectInteger(session.RunEntry(), kReloadedResult);
  EXPECT_EQ(session.generation(), 2U);
}

TEST(DevelopmentSessionTest, LoadAddsIndependentModulesAndTheirExportedEntrypoints) {
  auto created = DevelopmentSession::Create("main.vela");
  ASSERT_TRUE(created);
  auto& session = **created;
  ASSERT_EQ(Apply(session, "func main(): void {}", DevelopmentOperation::Load).status,
            DevelopmentStatus::Accepted);
  auto candidate = session.Prepare(
      {
          .base_revision = 1,
          .changes = {{.path = "plugin.vela", .text = "export func answer(): int { return 42; }"}},
          .added_roots = {"plugin.vela"},
      },
      DevelopmentOperation::Load);
  ASSERT_TRUE(candidate);
  ASSERT_EQ(session.Submit(std::move(*candidate)).status, DevelopmentStatus::WaitingForSafepoint);
  ASSERT_EQ(session.Pump().status, DevelopmentStatus::Accepted);
  EXPECT_EQ(session.Invoke("plugin.vela", "answer")->AsInteger(), 42);
}

TEST(DevelopmentSessionTest, StalePreparedCandidatesRecompileAgainstTheLatestCommittedSource) {
  auto created = DevelopmentSession::Create("main.vela");
  ASSERT_TRUE(created);
  auto& session = **created;
  ASSERT_EQ(Apply(session, "export func answer(): int { return 1; }").status,
            DevelopmentStatus::Accepted);
  auto first = session.Prepare(Edit(session, "export func answer(): int { return 2; }"),
                               DevelopmentOperation::HotReload);
  auto second = session.Prepare(Edit(session, "export func answer(): int { return 42; }"),
                                DevelopmentOperation::HotReload);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_EQ(session.Submit(std::move(*first)).status, DevelopmentStatus::WaitingForSafepoint);
  ASSERT_EQ(session.Pump().status, DevelopmentStatus::Accepted);
  ASSERT_EQ(session.Submit(std::move(*second)).status, DevelopmentStatus::WaitingForSafepoint);
  const auto result = session.Pump();
  ASSERT_EQ(result.status, DevelopmentStatus::Accepted) << result.message;
  EXPECT_EQ(result.revision, 3U);
  EXPECT_EQ(session.Invoke("main.vela", "answer")->AsInteger(), 42);
}

TEST(DevelopmentSessionTest, WaitingRequestsCanBeCancelledWithoutChangingEitherBaseline) {
  auto created = DevelopmentSession::Create("main.vela");
  ASSERT_TRUE(created);
  auto& session = **created;
  ASSERT_EQ(Apply(session, "export func answer(): int { return 42; }").status,
            DevelopmentStatus::Accepted);
  session.isolate().native_control().EnterHost();
  auto candidate = session.Prepare(Edit(session, "export func answer(): int { return 100; }"),
                                   DevelopmentOperation::HotReload);
  ASSERT_TRUE(candidate);
  ASSERT_EQ(session.Submit(std::move(*candidate)).status, DevelopmentStatus::WaitingForSafepoint);
  EXPECT_EQ(session.Pump().status, DevelopmentStatus::WaitingForSafepoint);
  ASSERT_TRUE(session.Cancel());
  session.isolate().native_control().ExitHost();
  EXPECT_EQ(session.Pump().status, DevelopmentStatus::Cancelled);
  EXPECT_EQ(session.snapshot()->revision, 1U);
  EXPECT_EQ(session.Invoke("main.vela", "answer")->AsInteger(), 42);
}

TEST(DevelopmentSessionTest, RefreshFailureReportsACommittedVersionAndDoesNotRollbackState) {
  std::uint32_t notifications = 0;
  DevelopmentOptions options;
  options.refreshed = FailingSecondRefresh(notifications);
  auto created = DevelopmentSession::Create("main.vela", {}, std::move(options));
  ASSERT_TRUE(created);
  auto& session = **created;
  ASSERT_EQ(Apply(session, "export func answer(): int { return 1; }").status,
            DevelopmentStatus::Accepted);
  const auto result = Apply(session, "export func answer(): int { return 42; }");
  EXPECT_EQ(result.status, DevelopmentStatus::RefreshFailed);
  EXPECT_EQ(result.execution, ExecutionStatus::TypeError);
  EXPECT_EQ(result.revision, 2U);
  EXPECT_EQ(notifications, 2U);
  EXPECT_EQ(session.Invoke("main.vela", "answer")->AsInteger(), 42);
}

TEST(DevelopmentSessionTest, ReloadWaitsForExternalRootsAndThenReexecutesEntryInANewRun) {
  auto created = DevelopmentSession::Create("main.vela");
  ASSERT_TRUE(created);
  auto& session = **created;
  ASSERT_EQ(
      Apply(session, "class Box {} export func make(): Box { return Box(); } func main(): void {}")
          .status,
      DevelopmentStatus::Accepted);
  std::optional<runtime::RootedValue> root;
  root.emplace(session.isolate(), *session.Invoke("main.vela", "make"));
  auto* previous = &session.isolate();
  auto result = Apply(session,
                      "let count = 100; func main(): void { count++; } "
                      "export func read(): int { return count; }",
                      DevelopmentOperation::Reload);
  ASSERT_EQ(result.status, DevelopmentStatus::WaitingForRelease) << result.message;
  ASSERT_FALSE(result.blockers.empty());
  EXPECT_EQ(result.blockers.front().kind, runtime::ExternalResourceKind::RootedValue);
  EXPECT_EQ(&session.isolate(), previous);
  EXPECT_EQ(session.generation(), 1U);
  root.reset();
  result = session.Pump();
  ASSERT_EQ(result.status, DevelopmentStatus::Accepted) << result.message;
  EXPECT_NE(&session.isolate(), previous);
  EXPECT_EQ(result.generation, 2U);
  EXPECT_EQ(result.revision, 1U);
  EXPECT_EQ(session.Invoke("main.vela", "read")->AsInteger(), 101);
}

struct DevelopmentHostState final {
  const std::function<void()>* action = nullptr;
  std::uint32_t finally_calls = 0;
  runtime::FutureCompletionEndpoint completion;
};

DevelopmentHostState& HostState() {
  static DevelopmentHostState state;
  return state;
}

void QueueReload() noexcept {
  (*HostState().action)();
}

void MarkFinally() noexcept {
  ++HostState().finally_calls;
}

TEST(DevelopmentSessionTest,
     ReloadUnwindsOldMachineFramesWithoutRunningFinallyOrDestroyingThemEarly) {
  runtime::Isolate registrations;
  ASSERT_TRUE(registrations.RegisterNative(runtime::NativeFunction::Bind<QueueReload>("queue")));
  ASSERT_TRUE(registrations.RegisterNative(runtime::NativeFunction::Bind<MarkFinally>("mark")));
  auto created = DevelopmentSession::Create("main.vela", registrations.FreezeBindings());
  ASSERT_TRUE(created);
  auto& session = **created;
  auto result = Apply(session,
                      "external func queue(): void; external func mark(): void; "
                      "export func work(): int { try { queue(); return 1; } finally { mark(); } } "
                      "func main(): void {}");
  ASSERT_EQ(result.status, DevelopmentStatus::Accepted) << result.message;
  HostState().finally_calls = 0;
  const auto action = QueueRequest(session,
                                   "external func queue(): void; external func mark(): void; "
                                   "let count = 40; func main(): void { count += 2; } "
                                   "export func read(): int { return count; }",
                                   DevelopmentOperation::Reload);
  HostState().action = &action;
  const auto executed = session.Invoke("main.vela", "work");
  HostState().action = nullptr;
  ASSERT_FALSE(executed);
  EXPECT_EQ(executed.error(), ExecutionStatus::ExecutionStopped);
  EXPECT_EQ(HostState().finally_calls, 0U);
  EXPECT_EQ(session.result().status, DevelopmentStatus::Accepted);
  EXPECT_EQ(session.generation(), 2U);
  EXPECT_EQ(session.Invoke("main.vela", "read")->AsInteger(), 42);
}

TEST(DevelopmentSessionTest, EntryFailureKeepsTheNewRunInstalledAndOldStateDiscarded) {
  auto created = DevelopmentSession::Create("main.vela");
  ASSERT_TRUE(created);
  auto& session = **created;
  ASSERT_EQ(Apply(session, "func main(): void {} export func answer(): int { return 1; }").status,
            DevelopmentStatus::Accepted);
  const auto result = Apply(session,
                            "class Failure {} func main(): void { throw Failure(); } "
                            "export func answer(): int { return 42; }",
                            DevelopmentOperation::Reload);
  EXPECT_EQ(result.status, DevelopmentStatus::EntryFailed);
  EXPECT_EQ(result.execution, ExecutionStatus::Thrown);
  EXPECT_EQ(session.generation(), 2U);
  EXPECT_EQ(session.Invoke("main.vela", "answer")->AsInteger(), 42);
}

runtime::Future StartRequest(runtime::NativeContext& context) noexcept {
  auto completer = context.CreateCompleter();
  const auto future = completer.future();
  HostState().completion = std::move(completer).Detach();
  return future;
}

TEST(DevelopmentSessionTest, LateHostCompletionCannotEnterTheReplacementRun) {
  runtime::Isolate registrations;
  ASSERT_TRUE(registrations.RegisterNative(runtime::NativeFunction::Bind<StartRequest>("request")));
  auto created = DevelopmentSession::Create("main.vela", registrations.FreezeBindings());
  ASSERT_TRUE(created);
  auto& session = **created;
  auto result =
      Apply(session,
            "external func request(): Future<int>; let value = 0; "
            "export async func start(): Future<int> { value = await request(); return value; } "
            "func main(): void {}");
  ExpectAcceptedSnapshot(session, result);
  ASSERT_TRUE(session.Invoke("main.vela", "start"));
  result = Apply(session,
                 "external func request(): Future<int>; let value = 42; "
                 "export func read(): int { return value; } func main(): void {}",
                 DevelopmentOperation::Reload);
  ExpectAcceptedSnapshot(session, result);
  std::expected<void, runtime::FutureCompletionError> completed;
  std::thread producer(
      [&] { completed = HostState().completion.Complete(runtime::Value::Integer(kAnswer)); });
  producer.join();
  ASSERT_FALSE(completed);
  EXPECT_EQ(completed.error(), runtime::FutureCompletionError::Closed);
  ExpectInteger(session.Invoke("main.vela", "read"), kAnswer);
}

TEST(DevelopmentSessionTest, BackgroundPreparationCanFinishWhileTheOwnerIsInsideAHostBinding) {
  runtime::Isolate registrations;
  ASSERT_TRUE(registrations.RegisterNative(runtime::NativeFunction::Bind<QueueReload>("queue")));
  auto created = DevelopmentSession::Create("main.vela", registrations.FreezeBindings());
  ASSERT_TRUE(created);
  auto& session = **created;
  ASSERT_EQ(Apply(session,
                  "external func queue(): void; export func work(): int { queue(); return 40; } "
                  "export func read(): int { return 1; }")
                .status,
            DevelopmentStatus::Accepted);
  const auto update = Edit(session,
                           "external func queue(): void; export func work(): int { return 100; } "
                           "export func read(): int { return 42; }");
  std::binary_semaphore entered{0};
  std::binary_semaphore compiled{0};
  std::promise<std::expected<DevelopmentCandidate, DevelopmentResult>> promise;
  auto prepared = promise.get_future();
  std::thread worker([&] {
    entered.acquire();
    promise.set_value(session.Prepare(update, DevelopmentOperation::HotReload));
    compiled.release();
  });
  std::function<void()> action = [&] {
    entered.release();
    compiled.acquire();
  };
  HostState().action = &action;
  const auto old = session.Invoke("main.vela", "work");
  HostState().action = nullptr;
  worker.join();
  ExpectInteger(old, kOldAnswer);
  auto candidate = prepared.get();
  ASSERT_TRUE(candidate);
  ASSERT_EQ(session.Submit(std::move(*candidate)).status, DevelopmentStatus::WaitingForSafepoint);
  ExpectAcceptedSnapshot(session, session.Pump());
  ExpectInteger(session.Invoke("main.vela", "read"), kAnswer);
}

TEST(DevelopmentSessionTest, NewlyRequiredGenericSpecializationsRecompileWithinTheSameRequest) {
  auto created = DevelopmentSession::Create("main.vela");
  ASSERT_TRUE(created);
  auto& session = **created;
  ASSERT_EQ(Apply(session,
                  "func value<T>(input: T): int { return 1; } "
                  "export func factory(): () => int { return () => value<int>(0); } "
                  "export func invoke(value: () => int): int { return value(); }")
                .status,
            DevelopmentStatus::Accepted);
  const runtime::RootedValue callback(session.isolate(), *session.Invoke("main.vela", "factory"));
  auto candidate =
      session.Prepare(Edit(session,
                           "func value<T>(input: T): int { return 42; } "
                           "export func factory(): () => int { return () => 0; } "
                           "export func invoke(value: () => int): int { return value(); }"),
                      DevelopmentOperation::HotReload);
  ASSERT_TRUE(candidate);
  const auto request = candidate->result().request;
  ASSERT_EQ(session.Submit(std::move(*candidate)).status, DevelopmentStatus::WaitingForSafepoint);
  EXPECT_EQ(session.Pump().status, DevelopmentStatus::WaitingForSafepoint);
  EXPECT_EQ(session.snapshot()->revision, 1U);
  const auto result = session.Pump();
  ASSERT_EQ(result.status, DevelopmentStatus::Accepted) << result.message;
  EXPECT_EQ(result.request, request);
  EXPECT_EQ(result.revision, 2U);
  const auto value = session.Invoke("main.vela", "invoke", std::array{callback.value()});
  ASSERT_TRUE(value);
  EXPECT_EQ(value->AsInteger(), 42);
}

}  // namespace
}  // namespace vela::compiler
