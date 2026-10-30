#include <algorithm>
#include <sstream>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/testing/result.h"
#include "compiler/testing/suite.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"

namespace vela::testing {
namespace {

class ScriptMockTest : public ::testing::TestWithParam<std::string_view> {};

TEST(ScriptMockLifetimeTest, CancellationClosesPendingObservationsAndReleasesAllRoots) {
  runtime::Isolate isolate(1);
  {
    const frontend::SourceInput input{
        .path = "cancel_test.vela",
        .text = R"(
import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('cancel', () => {
    const function = mock.MockFunction0<int>(mock.MockSession());
    mock.untilCalled(function.query());
    print('observing');
  });
})",
    };
    const auto compiled =
        compiler::CompileParsed(frontend::LoadSources({&input, 1}, input.path), isolate);
    ASSERT_TRUE(compiled.Succeeded());
    std::ostringstream output;
    isolate.SetOutput(output);
    SuiteOptions options;
    options.is_cancelled = [&output] { return output.str().contains("observing\n"); };
    Suite suite(isolate, compiled.program, std::move(options), output);
    const auto result = suite.Run();
    ASSERT_EQ(result.tests.size(), 1U);
    EXPECT_EQ(result.tests.front().status, TestStatus::Cancelled);
    EXPECT_TRUE(std::ranges::any_of(result.tests.front().errors, [](const auto& error) {
      return error.category == "MockSessionClosed";
    }));
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
  EXPECT_TRUE(isolate.ReloadBlockers().empty());
  EXPECT_FALSE(isolate.scheduler().PumpOne(runtime::FuturePumpMode::ReadyWork));
}

TEST_P(ScriptMockTest, ExecutesThroughSdkModulesAndTestScopesUnderGc) {
  runtime::Isolate isolate(1);
  std::ostringstream output;
  isolate.SetOutput(output);
  {
    const frontend::SourceInput input{.path = "mock_test.vela", .text = GetParam()};
    const auto compiled =
        compiler::CompileParsed(frontend::LoadSources({&input, 1}, input.path), isolate);
    for (const auto& diagnostic : compiled.diagnostics) {
      ADD_FAILURE() << diagnostic.source_path << ':' << diagnostic.location.line << ':'
                    << diagnostic.location.column << ' ' << diagnostic.message;
    }
    ASSERT_TRUE(compiled.Succeeded());
    Suite suite(isolate, compiled.program, {}, output);
    const auto result = suite.Run();
    EXPECT_EQ(result.exit_code, 0) << output.str();
    ASSERT_FALSE(result.tests.empty());
    for (const auto& test : result.tests) {
      for (const auto& error : test.errors) {
        ADD_FAILURE() << test.name << ": " << error.message;
      }
      EXPECT_EQ(test.status, TestStatus::Passed);
    }
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
  EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
  EXPECT_TRUE(isolate.ReloadBlockers().empty());
  EXPECT_FALSE(isolate.scheduler().PumpOne(runtime::FuturePumpMode::ReadyWork));
}

INSTANTIATE_TEST_SUITE_P(Core,
                         ScriptMockTest,
                         ::testing::Values(
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('priority, finite sequence and consumption', () => {
    const session = mock.MockSession();
    const function = mock.MockFunction1<int, string>(session);
    mock.when(function.query(mock.any)).thenReturn('default');
    mock.when(function.query(42)).thenReturnInOrder(['first', 'second']);
    expect(function.function(42), 'first');
    expect(function.function(1), 'default');
    expect(function.function(42), 'second');
    let exhausted = false;
    try { function.function(42); }
    catch (error: mock.MockStubExhausted) { exhausted = true; }
    expect(exhausted, true);
    expect(mock.verify(function.query(mock.captureAny)).called(4).captured, [42, 1, 42, 42]);
    mock.verifyNoMoreInteractions(function.control);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('void, arbitrary exceptions and response reentry', () => {
    const session = mock.MockSession();
    const sink = mock.MockVoidFunction1<string>(session);
    const source = mock.MockFunction0<int>(session);
    let text = '';
    mock.when(sink.query(mock.any)).thenDo((call: mock.MockInvocation): void => {
      text = call.argumentAt(0);
    });
    mock.when(source.query()).thenAnswer((call: mock.MockInvocation): int => {
      sink.function('answer');
      return 42;
    });
    expect(source.function(), 42);
    expect(text, 'answer');
    mock.verifyInOrder([source.query(), sink.query('answer')]);
    mock.verifyNoMoreInteractions(source.control);
    mock.verifyNoMoreInteractions(sink.control);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('failed verification rolls back and snapshots exclude future calls', () => {
    const session = mock.MockSession();
    const function = mock.MockFunction1<int, int>(session);
    mock.when(function.query(mock.any)).thenReturn(42);
    function.function(1);
    const snapshot = mock.verify(function.query(mock.captureAny));
    function.function(2);
    let failed = false;
    try { snapshot.called(2); }
    catch (error: mock.MockVerificationFailure) { failed = true; }
    expect(failed, true);
    expect(snapshot.called(1).captured, [1]);
    expect(mock.verify(function.query(mock.captureAny)).called(greaterThan(0)).captured, [2]);
    mock.verifyNever(function.query(mock.any));
    mock.clearInteractions(function.control);
    mock.verifyZeroInteractions(function.control);
    expect(function.function(3), 42);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
let function: mock.MockFunction0<int>? = null;
func main(): void {
  setUp(() => {
    const current = mock.MockFunction0<int>(mock.MockSession());
    function = current;
    mock.when(current.query()).thenReturn(42);
    addTearDown(() => { expect(current.function(), 42); });
  });
  tearDown(() => {
    const current = function as mock.MockFunction0<int>;
    mock.verify(current.query()).called(2);
    mock.verifyNoMoreInteractions(current.control);
  });
  test('session remains usable through all cleanup', () => {
    const current = function as mock.MockFunction0<int>;
    expect(current.function(), 42);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('Future responses and observations preserve call order', async (): Future<void> => {
    const session = mock.MockSession();
    const function = mock.MockFunction1<int, Future<int>>(session);
    mock.when(function.query(mock.any)).thenAnswer(
      async (call: mock.MockInvocation): Future<int> => {
        await sleep(1);
        return (call.argumentAt(0) as int) + 40;
      });
    const first = mock.untilCalled(function.query(2));
    const second = mock.untilCalled(function.query(2));
    mock.clearInteractions(function.control);
    const answer = function.function(2);
    expect((await first).argumentAt(0), 2);
    expect((await second).sequence, 1);
    mock.verify(function.query(2)).called(1);
    expect((await mock.untilCalled(function.query(2))).sequence, 1);
    expect(await answer, 42);
    mock.verifyNoMoreInteractions(function.control);
  });
  test('unawaited observation keeps the test open', () => {
    const function = mock.MockVoidFunction0(mock.MockSession());
    mock.when(function.query()).thenDo((call: mock.MockInvocation): void => {});
    mock.untilCalled(function.query());
    Timer.after(1, () => { function.function(); });
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
class BusinessError { code: int; }
func main(): void {
  test('errors retain identity and failed calls remain visible', () => {
    const function = mock.MockFunction0<int>(mock.MockSession());
    const original = BusinessError(code: 42);
    mock.when(function.query()).thenThrow(original);
    expect((): int => function.function(), throwsA(same(original)));
    mock.verify(function.query()).called(1);
    mock.reset(function.control);
    expect((): int => function.function(), throwsA(predicate(
      (value: dynamic): bool => value is mock.MockMissingStub, 'missing stub')));
    mock.verify(function.query()).called(1);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('configuration validates signatures without executing them', () => {
    const usage = predicate((error: dynamic): bool => error is mock.MockUsageError, 'usage error');
    const function = mock.MockFunction1<int, int>(mock.MockSession());
    const builder = mock.when(function.query(mock.any));
    expect(() => { builder.thenReturn('wrong'); }, throwsA(usage));
    expect(() => { builder.thenReturnInOrder([]); }, throwsA(usage));
    expect(() => { builder.thenReturn(Future.value(42)); }, throwsA(usage));
    expect(() => { builder.thenThrow(null); }, throwsA(usage));
    expect(() => { builder.thenAnswer(42); }, throwsA(usage));
    let calls = 0;
    expect(() => { builder.thenAnswer((value: int): int => { calls++; return 1; }); }, throwsA(usage));
    expect(() => { builder.thenAnswer((value: mock.MockInvocation): string => 'wrong'); }, throwsA(usage));
    expect(() => { builder.thenDo((value: mock.MockInvocation): void => { calls++; }); }, throwsA(usage));
    expect(calls, 0);
    builder.thenReturn(42);
    expect(() => { builder.thenReturn(43); }, throwsA(usage));
    expect(() => { mock.when(function.query(mock.captureAny)); }, throwsA(usage));
    expect(() => { mock.untilCalled(function.query(mock.captureAny)); }, throwsA(usage));
    expect(function.function(1), 42);
    const verification = mock.verify(function.query(1));
    expect(() => { verification.called(-1); }, throwsA(usage));
    expect(() => { verification.called(1.0); }, throwsA(usage));
    verification.called(1);
    expect(() => { verification.called(1); }, throwsA(usage));
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('ordered verification rolls back across mocks and requires distinct calls', () => {
    const session = mock.MockSession();
    const first = mock.MockFunction1<int, int>(session);
    const second = mock.MockFunction0<int>(session);
    mock.when(first.query(mock.any)).thenReturn(42);
    mock.when(second.query()).thenReturn(7);
    first.function(1); second.function(); first.function(2);
    let failed = false;
    try { mock.verifyInOrder([first.query(mock.captureAny), first.query(3)]); }
    catch (error: mock.MockVerificationFailure) { failed = true; }
    expect(failed, true);
    const result = mock.verifyInOrder([first.query(mock.captureAny), second.query(), first.query(mock.captureAny)]);
    expect(result.captured, [1, 2]);
    expect(result.invocations[0].sequence, 1);
    expect(result.invocations[1].sequence, 2);
    expect(result.invocations[2].sequence, 3);
    mock.verifyNoMoreInteractions(first.control);
    mock.verifyNoMoreInteractions(second.control);
    mock.clearInteractions(first.control);
    first.function(4); first.function(4);
    const repeated = mock.verifyInOrder([first.query(4), first.query(4)]);
    expect(repeated.invocations[0].sequence, 4);
    expect(repeated.invocations[1].sequence, 5);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('matchers execute once, preserve exceptions and block session mutation', () => {
    const function = mock.MockFunction1<int, int>(mock.MockSession());
    let comparisons = 0;
    mock.when(function.query(predicate((value: dynamic): bool => {
      comparisons++; return value == 1;
    }, 'one'))).thenReturn(42);
    expect(function.function(1), 42);
    expect(comparisons, 1);
    let missing = false;
    try { function.function(2); }
    catch (error: mock.MockMissingStub) { missing = true; }
    expect(missing, true);
    expect(comparisons, 2);
    mock.when(function.query(predicate((value: dynamic): bool => {
      mock.clearInteractions(function.control); return true;
    }, 'mutating predicate'))).thenReturn(3);
    let rejected = false;
    try { function.function(3); }
    catch (error: mock.MockUsageError) { rejected = true; }
    expect(rejected, true);
    expect(comparisons, 2);
    mock.verify(function.query(mock.any)).called(3);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('captured arrays retain identity and public arrays cannot mutate records', () => {
    const function = mock.MockFunction1<dynamic, int>(mock.MockSession());
    mock.when(function.query(mock.any)).thenReturn(42);
    const argument: dynamic[] = [null, 1];
    argument[0] = argument;
    function.function(argument);
    argument[1] = 2;
    const result = mock.verify(function.query(mock.captureThat(same(argument)))).called(1);
    expect(result.captured.length, 1);
    expect(result.captured[0], same(argument));
    const captured = result.captured;
    captured[0] = null;
    expect(result.captured[0], same(argument));
    const invocation = result.invocations[0];
    const arguments = invocation.arguments;
    arguments[0] = null;
    expect(invocation.argument('argument'), same(argument));
    expect(invocation.callLocation.length, greaterThan(0));
    expect(invocation.registrationLocation.length, greaterThan(0));
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('recursive responses share sequence and stale builders cannot commit', () => {
    const session = mock.MockSession();
    const function = mock.MockFunction1<int, int>(session);
    mock.when(function.query(mock.any)).thenAnswer((call: mock.MockInvocation): int => {
      const argument = call.argumentAt(0) as int;
      return argument == 0 ? 0 : 1 + function.function(argument - 1);
    });
    expect(function.function(2), 2);
    const old = mock.verify(function.query(mock.any));
    const result = mock.verifyInOrder([function.query(2), function.query(1), function.query(0)]);
    expect(result.invocations[2].sequence, 3);
    const usage = predicate((error: dynamic): bool => error is mock.MockUsageError, 'usage error');
    expect(() => { old.called(3); }, throwsA(usage));
    const cleared = mock.verify(function.query(mock.any));
    mock.clearInteractions(function.control);
    expect(() => { cleared.called(0); }, throwsA(usage));
    const pending = mock.when(function.query(5));
    mock.reset(function.control);
    expect(() => { pending.thenReturn(1); }, throwsA(usage));
    const other = mock.MockFunction0<int>(mock.MockSession());
    expect(() => { mock.verifyInOrder([function.query(1), other.query()]); }, throwsA(usage));
    mock.when(function.query(mock.any)).thenReturn(42);
    function.function(1);
    expect(() => { mock.verifyInOrder([function.query(1), other.query()]); }, throwsA(usage));
    mock.verify(function.query(1)).called(1);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
let inspected = 0;
class BusinessValue {
  get dangerous(): string { inspected++; throw Error(message: 'getter executed'); }
  toJson(): dynamic { inspected++; throw Error(message: 'conversion executed'); }
}
func main(): void {
  test('finite sequences survive clear and diagnostic display has no user effects', () => {
    const session = mock.MockSession();
    const function = mock.MockFunction0<int>(session);
    mock.when(function.query()).thenReturnInOrder([41, 42]);
    expect(function.function(), 41);
    mock.clearInteractions(function.control);
    expect(function.function(), 42);
    const value = BusinessValue();
    const other = mock.MockFunction1<BusinessValue, int>(session);
    mock.when(other.query(value)).thenReturn(42);
    try { other.function(BusinessValue()); } catch (error: mock.MockMissingStub) {}
    expect(inspected, 0);
    mock.verify(other.query(mock.any)).called(1);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
class Failure { code: int; }
func main(): void {
  test('Future adoption, failure identity and listeners use normal microtasks', async (): Future<void> => {
    const session = mock.MockSession();
    const function = mock.MockFunction0<Future<int>>(session);
    const completion = Completer<int>();
    const adopted = Completer<int>();
    adopted.complete(completion.future);
    mock.when(function.query()).thenAnswer((call: mock.MockInvocation): Future<int> => adopted.future);
    const observed = mock.untilCalled(function.query());
    let notified = false;
    observed.then((call: mock.MockInvocation): void => { notified = true; });
    const result = function.function();
    expect(notified, false);
    expect((await observed).sequence, 1);
    expect(notified, true);
    completion.complete(42);
    expect(await result, 42);
    const original = Failure(code: 7);
    mock.when(function.query()).thenAnswer((call: mock.MockInvocation): Future<int> => Future.error<int>(original));
    let sameError = false;
    try { await function.function(); }
    catch (error: Failure) { sameError = same(original).match(error).success; }
    expect(sameError, true);
    mock.verify(function.query()).called(2);
  });
})",
                             R"(import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('reusing one immutable pattern evaluates each candidate matcher once', () => {
    const function = mock.MockFunction1<int, int>(mock.MockSession());
    let comparisons = 0;
    const pattern = function.query(predicate((value: dynamic): bool => {
      comparisons++; return value == 42;
    }, 'forty two'));
    mock.when(pattern).thenReturn(1);
    mock.when(pattern).thenReturn(2);
    expect(function.function(42), 2);
    expect(comparisons, 1);
    let missing = false;
    try { function.function(0); }
    catch (error: mock.MockMissingStub) { missing = true; }
    expect(missing, true);
    expect(comparisons, 2);
    mock.verify(function.query(mock.any)).called(2);
  });
})"));

}  // namespace
}  // namespace vela::testing
