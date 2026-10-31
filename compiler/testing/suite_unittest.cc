#include "compiler/testing/suite.h"

#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "compiler/frontend/diagnostic.h"
#include "compiler/testing/result.h"
#include "gtest/gtest.h"

#include "compiler/bytecode/compiler.h"
#include "compiler/frontend/modules/source_loader.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/value.h"

namespace vela::testing {
namespace {

SuiteResult Execute(std::string_view source, std::ostream& output, SuiteOptions options = {}) {
  runtime::Isolate isolate(1);
  isolate.SetOutput(output);
  const frontend::SourceInput input{.path = "suite.vela", .text = source};
  const auto compiled =
      compiler::CompileParsed(frontend::LoadSources({&input, 1}, input.path), isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    EXPECT_NE(diagnostic.severity, frontend::DiagnosticSeverity::Error)
        << diagnostic.source_path << ':' << diagnostic.location.line << ' ' << diagnostic.message;
  }
  if (!compiled.Succeeded()) {
    return {};
  }
  Suite suite(isolate, compiled.program, std::move(options), output);
  return suite.Run();
}

TEST(ScriptSuiteTest, PreservesNestedLifecycleOrderAndRecursiveCleanupUnderGc) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  setUpAll(() => {
    print('root all');
    addTearDown(() => { print('root dynamic all'); });
  });
  setUp(() => { print('root setup'); });
  tearDown(() => { print('root teardown'); });
  tearDownAll(() => { print('root end'); });
  group('outer', () => {
    setUpAll(() => { print('outer all'); });
    tearDownAll(() => { print('outer end'); });
    setUp(() => { print('outer setup'); addTearDown(() => { print('setup cleanup'); }); });
    tearDown(() => { print('outer teardown first'); });
    tearDown(() => { print('outer teardown second'); });
    for (let index = 0; index < 2; index++) {
      test('same', () => {
        print('body');
        addTearDown(() => { print('dynamic first'); });
        addTearDown(() => {
          print('dynamic second');
          addTearDown(() => { print('dynamic recursive'); });
        });
      });
    }
  });
})",
                              output);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 2U);
  EXPECT_EQ(result.tests.at(0).status, TestStatus::Passed);
  EXPECT_EQ(result.tests.at(1).status, TestStatus::Passed);
  EXPECT_NE(result.tests.at(0).id, result.tests.at(1).id);
  EXPECT_EQ(result.tests.at(0).name, "outer same");
  constexpr std::string_view kEach =
      "root setup\nouter setup\nbody\ndynamic second\ndynamic recursive\n"
      "dynamic first\nsetup cleanup\nouter teardown second\n"
      "outer teardown first\nroot teardown\n";
  EXPECT_EQ(output.str(), "root all\nouter all\n" + std::string(kEach) + std::string(kEach) +
                              "outer end\nroot dynamic all\nroot end\n");
}

TEST(ScriptSuiteTest, RetainsInitialFailureAndEveryCleanupFailure) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  tearDown(() => { fail('static cleanup'); });
  test('fails', () => {
    addTearDown(() => { fail('first cleanup'); });
    addTearDown(() => { fail('second cleanup'); });
    fail('body failure');
  });
  test('after', () => { print('after body'); });
})",
                              output);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 2U);
  const auto& first = result.tests.front();
  EXPECT_EQ(first.status, TestStatus::Failed);
  ASSERT_EQ(first.errors.size(), 4U);
  EXPECT_EQ(first.errors.at(0).message, "body failure");
  EXPECT_EQ(first.errors.at(1).message, "second cleanup");
  EXPECT_EQ(first.errors.at(2).message, "first cleanup");
  EXPECT_EQ(first.errors.at(3).message, "static cleanup");
  EXPECT_EQ(output.str(), "after body\n");
}

TEST(ScriptSuiteTest, FiltersBeforeHooksAndListsWithoutExecutingBodies) {
  std::ostringstream output;
  SuiteOptions options;
  options.list = true;
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  print('register');
  setUpAll(() => { fail('must not run'); });
  group('group', () => {
    test('one', () => { fail('must not run'); }, tags: ['unit']);
    test('two', () => { fail('must not run'); }, skip: 'reason');
  });
})",
                              output, std::move(options));
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 2U);
  EXPECT_EQ(result.tests.back().status, TestStatus::Skipped);
  EXPECT_EQ(result.tests.back().skip_reason, "reason");
  EXPECT_EQ(output.str(), "register\n");
}

TEST(ScriptSuiteTest, RejectsInvalidCallbackContractsAtRegistration) {
  for (const auto* source : {
           "import 'sdk:test'; func main(): void { test('bad', null); }",
           "import 'sdk:test'; func main(): void { test('bad', (x: int): void => {}); }",
           "import 'sdk:test'; func main(): void { group('bad', async (): Future<void> => {}); }",
           "import 'sdk:test'; func main(): void { group('bad', (): int => 42); }",
       }) {
    std::ostringstream output;
    const auto result = Execute(source, output);
    ASSERT_EQ(result.errors.size(), 1U) << source;
    EXPECT_EQ(result.errors.front().phase, TestPhase::Registration);
    EXPECT_TRUE(result.tests.empty());
  }
}

TEST(ScriptSuiteTest, WaitsForAsyncMainOrdinaryResultsAndAsyncCleanup) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
async func main(): Future<void> {
  await sleep(0);
  setUp(async (): Future<int> => { await sleep(0); print('setup'); return 42; });
  tearDown(async (): Future<void> => { await sleep(0); print('cleanup'); });
  test('async', async (): Future<int> => { await sleep(0); print('async body'); return 42; });
  test('sync value', (): int => 42);
})",
                              output);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 2U);
  EXPECT_EQ(result.tests.at(0).status, TestStatus::Passed);
  EXPECT_EQ(result.tests.at(1).status, TestStatus::Passed);
  EXPECT_EQ(output.str(), "setup\nasync body\ncleanup\nsetup\ncleanup\n");
}

TEST(ScriptSuiteTest, WaitsForExpectLaterEvenWhenTheCallerDoesNotAwait) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  test('success', () => {
    expect(Future.value(42), completion(42));
    expectLater(Future.delayed(0, (): int => 43), completion(43));
  });
  test('failure', () => { expectLater(Future.value(42), completion(43)); });
  test('awaited failure', async (): Future<void> => {
    await expectLater(Future.value(42), completion(43));
  });
  test('after', () => { print('after'); });
})",
                              output);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 4U);
  EXPECT_EQ(result.tests.at(0).status, TestStatus::Passed);
  EXPECT_EQ(result.tests.at(1).status, TestStatus::Failed);
  ASSERT_EQ(result.tests.at(1).errors.size(), 1U);
  EXPECT_EQ(result.tests.at(2).status, TestStatus::Failed);
  ASSERT_EQ(result.tests.at(2).errors.size(), 1U);
  EXPECT_EQ(result.tests.at(3).status, TestStatus::Passed);
  EXPECT_EQ(output.str(), "after\n");
}

TEST(ScriptSuiteTest, TracksCallbackCountsAndExplicitSingleArgumentType) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  test('callbacks', () => {
    const zero = expectAsync0(() => { print('zero'); }, count: 2);
    const one = expectAsync1<int>((value: int): void => { expect(value, 42); print('one'); });
    Timer.after(0, zero);
    Timer.after(1, zero);
    Timer.after(0, () => { one(42); });
  });
  test('missing', () => { expectAsync0(() => {}); }, timeout: Timeout(0));
  test('excess', () => { const callback = expectAsync0(() => {}); callback(); callback(); });
})",
                              output);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 3U);
  EXPECT_EQ(result.tests.at(0).status, TestStatus::Passed);
  EXPECT_EQ(result.tests.at(1).status, TestStatus::Failed);
  ASSERT_EQ(result.tests.at(1).errors.size(), 1U);
  EXPECT_NE(result.tests.at(1).errors.front().message.find("TestTimeout"), std::string::npos);
  EXPECT_EQ(result.tests.at(2).status, TestStatus::Failed);
  ASSERT_EQ(result.tests.at(2).errors.size(), 1U);
  EXPECT_NE(result.tests.at(2).errors.front().message.find("more than max"), std::string::npos);
  EXPECT_EQ(output.str(), "zero\none\nzero\n");
}

TEST(ScriptSuiteTest, AttributesLateTimerAndListenerErrorsToTheOriginalTest) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
let producer: Completer<int> = Completer<int>();
func main(): void {
  test('timer owner', () => { Timer.after(1, () => { fail('late timer'); }); });
  test('listener owner', () => {
    producer.future.then((value: int): int => { fail('late listener'); return value; });
  });
  test('next', async (): Future<void> => { producer.complete(42); await sleep(5); });
})",
                              output);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 3U);
  EXPECT_EQ(result.tests.at(0).status, TestStatus::Failed);
  ASSERT_EQ(result.tests.at(0).errors.size(), 1U);
  EXPECT_EQ(result.tests.at(0).errors.front().message, "late timer");
  EXPECT_EQ(result.tests.at(1).status, TestStatus::Failed);
  ASSERT_EQ(result.tests.at(1).errors.size(), 1U);
  EXPECT_EQ(result.tests.at(1).errors.front().message, "late listener");
  EXPECT_EQ(result.tests.at(2).status, TestStatus::Passed);
}

TEST(ScriptSuiteTest, TimesOutWithoutCancellingTheFutureAndStillRunsCleanup) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  test('timeout', async (): Future<void> => {
    addTearDown(() => { print('cleanup'); });
    await sleep(5);
    fail('late after timeout');
  }, timeout: Timeout(0));
  test('next', async (): Future<void> => { await sleep(10); });
})",
                              output);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 2U);
  EXPECT_EQ(result.tests.at(0).status, TestStatus::Failed);
  ASSERT_EQ(result.tests.at(0).errors.size(), 2U);
  EXPECT_NE(result.tests.at(0).errors.at(0).message.find("TestTimeout"), std::string::npos);
  EXPECT_EQ(result.tests.at(0).errors.at(1).message, "late after timeout");
  EXPECT_EQ(result.tests.at(1).status, TestStatus::Passed);
  EXPECT_EQ(output.str(), "cleanup\n");
}

TEST(ScriptSuiteTest, CallbackProgressExtendsInactivityTimeoutBeyondTotalDuration) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  test('heartbeat', async (): Future<void> => {
    const callback = expectAsync0(() => {}, count: 1, max: 20);
    const timer = Timer.periodic(10, callback);
    addTearDown(() => { timer.cancel(); });
    await sleep(80);
  }, timeout: Timeout(40));
})",
                              output);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 1U);
  EXPECT_EQ(result.tests.front().status, TestStatus::Passed);
}

TEST(ScriptSuiteTest, BlocksDescendantsAfterSetUpAllFailureAndRetainsGroupCleanup) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  group('failed', () => {
    setUpAll(() => { addTearDown(() => { print('dynamic all'); }); fail('setup all'); });
    tearDownAll(() => { print('static all'); });
    test('one', () => { fail('body must not execute'); });
    test('two', () => { fail('body must not execute'); });
  });
  test('independent', () => { print('independent'); });
})",
                              output);
  ASSERT_EQ(result.errors.size(), 1U);
  EXPECT_EQ(result.errors.front().message, "setup all");
  ASSERT_EQ(result.tests.size(), 3U);
  EXPECT_EQ(result.tests.at(0).status, TestStatus::Failed);
  EXPECT_EQ(result.tests.at(1).status, TestStatus::Failed);
  EXPECT_EQ(result.tests.at(2).status, TestStatus::Passed);
  EXPECT_EQ(output.str(), "dynamic all\nstatic all\nindependent\n");
}

TEST(ScriptSuiteTest, CancellationStopsAtAwaitBoundariesAndStillAwaitsCleanup) {
  std::ostringstream output;
  SuiteOptions options;
  options.is_cancelled = [&output] { return output.str().contains("started\n"); };
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  tearDown(async (): Future<void> => { await sleep(1); print('cleanup'); });
  test('active', async (): Future<void> => { print('started'); await sleep(10000); fail('cancelled work'); });
  test('pending', () => { fail('must not start'); });
})",
                              output, std::move(options));
  ASSERT_EQ(result.tests.size(), 2U);
  EXPECT_EQ(result.tests.at(0).status, TestStatus::Cancelled);
  EXPECT_EQ(result.tests.at(1).status, TestStatus::Cancelled);
  EXPECT_EQ(output.str(), "started\ncleanup\n");
}

TEST(ScriptSuiteTest, PartialSetupFailureCleansOnlyEnteredScopesAndKeepsAllErrors) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
func main(): void {
  setUp(() => { print('outer setup'); addTearDown(() => { print('dynamic'); }); fail('setup'); });
  tearDown(() => { print('outer cleanup'); fail('cleanup'); });
  group('inner', () => {
    setUp(() => { fail('inner setup must not run'); });
    tearDown(() => { fail('inner cleanup must not run'); });
    test('blocked', () => { fail('body must not run'); });
  });
})",
                              output);
  ASSERT_EQ(result.tests.size(), 1U);
  const auto& errors = result.tests.front().errors;
  ASSERT_EQ(errors.size(), 2U);
  EXPECT_EQ(errors.at(0).message, "setup");
  EXPECT_EQ(errors.at(1).message, "cleanup");
  EXPECT_EQ(output.str(), "outer setup\ndynamic\nouter cleanup\n");
}

TEST(ScriptSuiteTest, FutureAdoptionMultipleListenersAndCatchFinallyKeepTheirScope) {
  std::ostringstream output;
  const auto result = Execute(R"(
import 'sdk:test';
class Failure { value: int; }
async func rejects(value: Failure): Future<int> { throw value; }
async func catches(value: Failure): Future<int> {
  try { await rejects(value); }
  catch (error: dynamic) { expect(error, same(value)); await sleep(1); }
  finally { await sleep(1); print('finally'); }
  return 42;
}
func main(): void {
  test('adoption', () => {
    const producer = Completer<int>();
    const adopted = Completer<int>();
    adopted.complete(producer.future);
    expectLater(adopted.future, completion(42));
    expectLater(producer.future.then((value: int): int => value + 1), completion(43));
    expectLater(catches(Failure(value: 1)), completion(42));
    Timer.after(0, () => { producer.complete(42); });
  });
})",
                              output);
  ASSERT_TRUE(result.errors.empty());
  ASSERT_EQ(result.tests.size(), 1U);
  EXPECT_EQ(result.tests.front().status, TestStatus::Passed);
  EXPECT_TRUE(result.tests.front().errors.empty());
  EXPECT_EQ(output.str(), "finally\n");
}

TEST(ScriptSuiteTest, ClosingSuiteReleasesTimersFramesRootsAndHostEndpoints) {
  runtime::Isolate isolate(1);
  auto marker = std::make_shared<int>(0);
  const std::weak_ptr<int> lifetime(marker);
  auto completer = isolate.scheduler().CreateHostCompleter();
  completer.future().Observe([marker](runtime::FutureState, runtime::Value) { ++*marker; });
  const auto endpoint = std::move(completer).Detach();
  marker.reset();
  std::ostringstream output;
  {
    const auto compiled = compiler::CompileSource(R"(
import 'sdk:test';
async func background(): Future<void> { await sleep(100000); fail('closed'); }
func main(): void {
  test('starts background work', () => {
    background();
    Timer.periodic(100000, () => { fail('closed timer'); });
  });
})",
                                                  isolate);
    ASSERT_TRUE(compiled.Succeeded());
    {
      Suite suite(isolate, compiled.program, {}, output);
      const auto result = suite.Run();
      ASSERT_EQ(result.tests.size(), 1U);
      EXPECT_EQ(result.tests.front().status, TestStatus::Passed);
      EXPECT_TRUE(lifetime.expired());
      const auto completed = endpoint.Complete(runtime::Value::Null());
      ASSERT_FALSE(completed);
      EXPECT_EQ(completed.error(), runtime::FutureCompletionError::Closed);
      EXPECT_FALSE(isolate.scheduler().PumpOne(runtime::FuturePumpMode::ReadyWork));
    }
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
  EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
  EXPECT_TRUE(isolate.ReloadBlockers().empty());
  EXPECT_TRUE(isolate.TryStop());
}

}  // namespace
}  // namespace vela::testing
