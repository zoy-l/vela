#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/bytecode_image.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

struct AsyncCase final {
  std::string_view name;
  std::string_view source;
  std::int64_t expected;
};

class AsyncContractTest : public testing::TestWithParam<AsyncCase> {};

void ExpectFutureInteger(const runtime::Future& result, std::int64_t expected) {
  ASSERT_EQ(result.state(), runtime::FutureState::Succeeded)
      << result.error_diagnostic().value_or(runtime::FutureDiagnostic{}).message;
  const auto value = result.value().value_or(runtime::Value::Null());
  ASSERT_TRUE(value.IsInteger());
  EXPECT_EQ(value.AsInteger(), expected);
}

TEST_P(AsyncContractTest, ExecutesThroughCheckedBytecode) {
  const auto& test = GetParam();
  runtime::Isolate isolate(1);
  isolate.scheduler().SetUnhandledErrorHandler([](const runtime::Future& future, runtime::Value) {
    ADD_FAILURE() << "unhandled callback: "
                  << future.error_diagnostic().value_or(runtime::FutureDiagnostic{}).message;
  });
  const auto compiled = CompileSource(test.source, isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  auto result = vm::VirtualMachine(isolate).ExecuteFuture(compiled.program, main.index());
  isolate.heap().Collect();
  ASSERT_TRUE(isolate.scheduler().PumpUntil(result));
  ExpectFutureInteger(result, test.expected);
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0);
}

TEST_P(AsyncContractTest, LoadsWithoutCompilerStateInFreshIsolate) {
  const auto& test = GetParam();
  std::vector<std::byte> image;
  {
    runtime::Isolate isolate;
    const auto compiled = CompileSource(test.source, isolate);
    ASSERT_TRUE(compiled.Succeeded());
    const auto saved = vm::SaveBytecode(compiled.program, isolate);
    ASSERT_TRUE(saved) << saved.error().message;
    image = *saved;
  }
  runtime::Isolate isolate(1);
  const auto program = vm::LoadBytecode(image, isolate);
  ASSERT_TRUE(program) << program.error().message;
  image.clear();
  auto result =
      vm::VirtualMachine(isolate).ExecuteFuture(*program, program->FindFunction("main").index());
  isolate.heap().Collect();
  ASSERT_TRUE(isolate.scheduler().PumpUntil(result));
  ExpectFutureInteger(result, test.expected);
}

TEST(AsyncLifetimeTest, UnreachablePendingFramesAreCollected) {
  runtime::Isolate isolate(1);
  {
    const auto compiled = CompileSource(R"(
async func orphan(): Future<void> { await Completer<void>().future; }
func main(): int { orphan(); return 0; }
)",
                                        isolate);
    ASSERT_TRUE(compiled.Succeeded());
    ASSERT_TRUE(
        vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main")).Succeeded());
    EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 1);
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0);
  EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0);
  EXPECT_EQ(isolate.heap().object_count(), 0);
}

TEST(AsyncLifetimeTest, RepeatedAwaitAllocatesOneResultFuturePerActivation) {
  runtime::Isolate isolate(std::numeric_limits<std::size_t>::max());
  const auto compiled = CompileSource(R"(
async func main(): Future<int> {
  for (let index = 0; index < 64; index++) { await Future.value(index); }
  return 42;
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto baseline = isolate.heap().object_count();
  auto result = vm::VirtualMachine(isolate).ExecuteFuture(
      compiled.program, compiled.program.FindFunction("main").index());
  ASSERT_TRUE(isolate.scheduler().PumpUntil(result));
  ExpectFutureInteger(result, 42);
  EXPECT_EQ(isolate.heap().object_count() - baseline, 65U);
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
  EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
}

TEST(AsyncLifetimeTest, UnreachableResuspendedActivationIsCollected) {
  runtime::Isolate isolate(1);
  {
    const auto compiled = CompileSource(R"(
async func orphan(): Future<void> { await 0; await Completer<void>().future; }
func main(): int { orphan(); return 0; }
)",
                                        isolate);
    ASSERT_TRUE(compiled.Succeeded());
    ASSERT_TRUE(
        vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main")).Succeeded());
    ASSERT_TRUE(isolate.scheduler().PumpOne());
    EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 1U);
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
  EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(AsyncLifetimeTest, PendingGlobalProducerDoesNotRootItsOwnProgram) {
  runtime::Isolate isolate(1);
  {
    const auto compiled = CompileSource(R"(
const producer = Completer<void>();
async func task(): Future<void> { await producer.future; }
func main(): int { task(); return 0; }
)",
                                        isolate);
    ASSERT_TRUE(compiled.Succeeded());
    ASSERT_TRUE(
        vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main")).Succeeded());
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0);
  EXPECT_EQ(isolate.heap().object_count(), 0);
}

TEST(AsyncLifetimeTest, EmptyRaceReportsIdleWithoutKeepingProcessAlive) {
  runtime::Isolate isolate;
  const auto compiled =
      CompileSource("func main(): Future<int> { return Future.race<int>([]); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  auto root = vm::VirtualMachine(isolate).ExecuteFuture(
      compiled.program, compiled.program.FindFunction("main").index());
  EXPECT_FALSE(isolate.scheduler().PumpUntil(root));
  EXPECT_EQ(root.state(), runtime::FutureState::Pending);
}

TEST(AsyncLifetimeTest, PendingAggregateAndCallbacksDoNotRootTheirOwnProgram) {
  runtime::Isolate isolate(1);
  {
    const auto compiled = CompileSource(R"(
const producer = Completer<int>();
func main(): int {
  Future.wait([producer.future, producer.future]).then(values => values.length);
  return 0;
}
)",
                                        isolate);
    ASSERT_TRUE(compiled.Succeeded());
    ASSERT_TRUE(
        vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main")).Succeeded());
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0);
}

INSTANTIATE_TEST_SUITE_P(
    AsyncSemantics,
    AsyncContractTest,
    testing::Values(AsyncCase{"CallerContinues", R"(
let order: int = 0;
async func child(): Future<int> {
  order = 1;
  await sleep(0);
  order = order * 10 + 3;
  return order;
}
async func main(): Future<int> {
  const task = child();
  order = order * 10 + 2;
  return await task;
})",
                              123},
                    AsyncCase{"PlainAwaitYields", R"(
let order: int = 0;
async func child(): Future<int> {
  order = 1;
  await 0;
  order = order * 10 + 3;
  return order;
}
async func main(): Future<int> {
  const task = child();
  order = order * 10 + 2;
  return await task;
})",
                              123},
                    AsyncCase{"NullableAwait", R"(
async func main(): Future<int> {
  const missing: Future<int>? = null;
  const value: int? = await missing;
  return value ?? 42;
})",
                              42},
                    AsyncCase{"AsyncThrowDoesNotEscape", R"(
async func fail(): Future<int> { throw Error(message: 'failed'); }
async func main(): Future<int> {
  let task: Future<int> = Future.value(0);
  try { task = fail(); } catch { return 1; }
  try { return await task; } catch (error: Error) { return 42; }
})",
                              42},
                    AsyncCase{"ThenInfersCallback", R"(
async func main(): Future<int> {
  return await Future.value(40).then((value) => value + 2);
})",
                              42},
                    AsyncCase{"ThenAdoptsCallbackFuture", R"(
async func add(value: int): Future<int> { await 0; return value + 2; }
async func main(): Future<int> { return await Future.value(40).then(add); }
)",
                              42},
                    AsyncCase{"SyncCapturesRuntimeError", R"(
async func main(): Future<int> {
  const task = Future.sync((): int => 1 / 0);
  return await task.catchError((error) => error.code == 'DivisionByZero' ? 42 : 0);
})",
                              42},
                    AsyncCase{"ThenErrorCallback", R"(
async func main(): Future<int> {
  return await Future.error<int>(Error(message: 'x')).then(
    (value) => value + 1, onError: (error) => 42);
})",
                              42},
                    AsyncCase{"TypedErrorCallback", R"(
class Failure { public param value: int; }
async func main(): Future<int> {
  return await Future.error<int>(Failure(value: 42)).catchError((error: Failure) => error.value);
})",
                              42},
                    AsyncCase{"UnmatchedErrorPropagates", R"(
class Failure {}
async func main(): Future<int> {
  try {
    return await Future.error<int>(Failure()).catchError((error: Error) => 1);
  } catch (error: Failure) { return 42; }
})",
                              42},
                    AsyncCase{"WhenCompleteAwaitsAction", R"(
let value: int = 0;
async func action(): Future<void> { await 0; value = 2; }
async func main(): Future<int> {
  const original = await Future.value(40).whenComplete(action);
  return original + value;
})",
                              42},
                    AsyncCase{"WhenCompletePreservesError", R"(
let value: int = 0;
async func main(): Future<int> {
  try {
    await Future.error<int>(Error(message: 'x')).whenComplete((): void => { value = 42; });
    return 0;
  } catch (error: Error) { return value; }
})",
                              42},
                    AsyncCase{"WhenCompleteActionErrorWins", R"(
func action(): void { throw Error(message: 'action', code: 'Action'); }
async func main(): Future<int> {
  try { return await Future.value(1).whenComplete(action); }
  catch (error: Error) { return error.code == 'Action' ? 42 : 0; }
})",
                              42},
                    AsyncCase{"CompleterAndDuplicateCompletion", R"(
async func main(): Future<int> {
  const completer = Completer<int>();
  const future = completer.future;
  completer.complete(40);
  let extra = 0;
  try { completer.complete(1); }
  catch (error: Error) { extra = error.code == 'FutureAlreadyCompleted' ? 2 : 0; }
  return await future + extra;
})",
                              42},
                    AsyncCase{"CompleterAdoptionClaimsCompletion", R"(
async func main(): Future<int> {
  const source = Completer<int>();
  const target = Completer<int>();
  target.complete(source.future);
  const claimed = target.isCompleted;
  source.complete(40);
  return await target.future + (claimed ? 2 : 0);
})",
                              42},
                    AsyncCase{"CompleterSelfAdoptionIsCatchable", R"(
async func main(): Future<int> {
  const value = Completer<int>();
  try { value.complete(value.future); }
  catch (error: Error) {
    value.complete(error.code == 'FutureSelfAdoption' ? 42 : 0);
  }
  return await value.future;
})",
                              42},
                    AsyncCase{"VoidCompleter", R"(
async func main(): Future<int> {
  const value = Completer<void>();
  value.complete();
  await value.future;
  return 42;
})",
                              42},
                    AsyncCase{"TimeoutReplacement", R"(
async func main(): Future<int> {
  const value = Completer<int>();
  return await value.future.timeout(0, onTimeout: () => 42);
})",
                              42},
                    AsyncCase{"TimeoutExceptionType", R"(
async func main(): Future<int> {
  try { return await Completer<int>().future.timeout(0); }
  catch (error: TimeoutException) { return 42; }
})",
                              42},
                    AsyncCase{"SourceTimeoutExceptionDoesNotRunReplacement", R"(
async func main(): Future<int> {
  let called = false;
  const source: Future<int> = Future.error(TimeoutException());
  try { return await source.timeout(0, onTimeout: (): int => { called = true; return 1; }); }
  catch (error: TimeoutException) { return called ? 0 : 42; }
})",
                              42},
                    AsyncCase{"WaitCleansSuccessesBeforeAndAfterError", R"(
async func main(): Future<int> {
  let sum = 0;
  const later = Completer<int>();
  const eager = true;
  const input: Future<int>[] = [Future.value(20), Future.error(Error(message: 'x')), later.future];
  const all = Future.wait(input, eagerError: eager, cleanUp: (value): void => { sum += value; });
  try { await all; } catch {}
  later.complete(22);
  await later.future;
  return sum;
})",
                              42},
                    AsyncCase{"EmptyWait", R"(
async func main(): Future<int> {
  const empty = await Future.wait<int>([]);
  return empty.length + 42;
})",
                              42},
                    AsyncCase{"CancelledTimerDoesNotFire", R"(
async func main(): Future<int> {
  let value = 42;
  const timer = Timer.after(0, (): void => { value = 0; });
  timer.cancel();
  await sleep(0);
  return value;
})",
                              42},
                    AsyncCase{"TimerCompletesProducer", R"(
async func main(): Future<int> {
  const result = Completer<int>();
  Timer.after(0, (): void => { result.complete(42); });
  return await result.future;
})",
                              42},
                    AsyncCase{"CancellationToken", R"(
async func main(): Future<int> {
  const token = CancellationToken();
  if (token.isCancelled) { return 0; }
  token.cancel();
  token.cancel();
  return token.isCancelled ? 42 : 0;
})",
                              42},
                    AsyncCase{"FutureMethodValue", R"(
async func main(): Future<int> {
  const source = Future.value(40);
  const then: ((int) => int) => Future<int> = source.then;
  return await then((value) => value + 2);
})",
                              42},
                    AsyncCase{"WaitForVoidTasks", R"(
async func main(): Future<int> {
  await Future.wait([sleep(0), sleep(0)]);
  await Future.wait<void>([]);
  return 42;
})",
                              42},
                    AsyncCase{"NullableValueFactory", R"(
async func main(): Future<int> {
  const pending: Future<int?> = Future.value(null);
  return await pending ?? 42;
})",
                              42},
                    AsyncCase{"OptionalCallbackVariable", R"(
async func main(): Future<int> {
  const callback: ((Error) => int)? = null;
  return await Future.value(42).then((value) => value, onError: callback);
})",
                              42},
                    AsyncCase{"FutureFieldsSurviveSuspension", R"(
class Holder { public param task: Future<int>; }
async func main(): Future<int> {
  const holder = Holder(task: Future.value(42));
  await 0;
  return await holder.task;
})",
                              42},
                    AsyncCase{"MicrotasksPrecedeTimers", R"(
async func main(): Future<int> {
  let order = 0;
  Timer.after(0, (): void => { order = order * 10 + 2; });
  Future.microtask((): void => { order = order * 10 + 4; });
  await sleep(0);
  return order;
})",
                              42},
                    AsyncCase{"FinallyAcrossAwait", R"(
let order = 0;
async func task(): Future<int> {
  try { await 0; return 40; }
  finally { await 0; order = 2; }
}
async func main(): Future<int> { const value = await task(); return value + order; }
)",
                              42},
                    AsyncCase{"ErrorCodeAcrossMultipleAwaits", R"(
async func fail(): Future<int> { await 0; return 1 / 0; }
async func middle(): Future<int> { return await fail(); }
async func main(): Future<int> {
  try { return await middle(); }
  catch (error: Error) { return error.code == 'DivisionByZero' ? 42 : 0; }
})",
                              42},
                    AsyncCase{"AsyncInterfaceMethodValue", R"(
interface Worker { run(): Future<int>; }
class Task implements Worker { public async run(): Future<int> { await 0; return 42; } }
async func main(): Future<int> {
  const worker: Worker = Task();
  const run = worker.run;
  return await run();
})",
                              42},
                    AsyncCase{"VoidThenCallback", R"(
async func main(): Future<int> {
  let value = 0;
  await sleep(0).then((): void => { value = 42; });
  return value;
})",
                              42},
                    AsyncCase{"CompleterMethodValue", R"(
async func main(): Future<int> {
  const completer = Completer<int>();
  const complete = completer.complete;
  complete(42);
  return await completer.future;
})",
                              42},
                    AsyncCase{"DynamicMemberGenerationPreservesSyncFutureIdentity", R"(
enum Choice { first, second }
async func main(): Future<int> {
  const selected = await Future.sync((): Choice => Choice.second);
  const token: dynamic = CancellationToken();
  token.cancel();
  return selected == Choice.second && token.isCancelled ? 42 : 0;
})",
                              42},
                    AsyncCase{"AwaitCompletionHasNoIntermediateForwardingTurn", R"(
async func input(): Future<int> { await 0; return 42; }
async func main(): Future<int> {
  let order = 0;
  const value = input();
  Future.microtask((): void => {
    order = order * 10 + 1;
    Future.microtask((): void => { order = order * 10 + 3; });
  });
  const result = await value;
  order = order * 10 + 2;
  return result == 42 && order == 12 ? 42 : 0;
})",
                              42},
                    AsyncCase{"RepeatedAwaitCompletesObjectWithoutForwardingTurns", R"(
class Box { public param value: int; }
let order = 0;
async func input(): Future<Box> {
  await 0;
  await 0;
  Future.microtask((): void => {
    order = order * 10 + 1;
    Future.microtask((): void => { order = order * 10 + 3; });
  });
  return Box(value: 42);
}
async func main(): Future<int> {
  const result = await input();
  order = order * 10 + 2;
  return result.value == 42 && order == 12 ? 42 : 0;
})",
                              42},
                    AsyncCase{"ExplicitComputationFactories", R"(
async func main(): Future<int> {
  const first = await Future.sync<int>(() => 10);
  const second = await Future.microtask<int>(() => Future.value(20));
  return first + second + await Future.delayed<int>(0, () => 12);
})",
                              42}),
    [](const testing::TestParamInfo<AsyncCase>& info) { return std::string(info.param.name); });

class AsyncTypeErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(AsyncTypeErrorTest, RejectsInvalidContract) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(GetParam(), isolate);
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_FALSE(compiled.diagnostics.empty());
}

INSTANTIATE_TEST_SUITE_P(
    AsyncTypes,
    AsyncTypeErrorTest,
    testing::Values(
        "func main(): Future<int> { return Future.value('wrong'); }",
        "func main(): Future<int> { return Future.error(42); }",
        "func main(): Future<int> { return Future.sync(() => 'wrong'); }",
        "func main(): void { Future.sync<int>(() => 'wrong'); }",
        "func main(): void { Future.microtask<int>(() => 'wrong'); }",
        "func main(): void { Future.delayed<int>(0, () => 'wrong'); }",
        "func main(): void { Future.sync<int, string>(() => 1); }",
        "func main(): Future<int> { return Future.microtask((value: int) => value); }",
        "func main(): Future<int[]> { return Future.wait<int>([], wrong: false); }",
        "func main(): Future<int> { return Future.race<int>([], eagerError: true); }",
        "func main(): Future<int> { return Future.value(1).then((value: string) => 1); }",
        "func main(): void { Future.value(1).then(value => missing); }",
        "func main(): Future<int> { return Future.value(1).catchError((error: int) => 1); }",
        "func main(): void { Completer<int>().complete('wrong'); }",
        "func main(): void { Completer<int>().completeError(null); }",
        "func main(): Future<int> { return Future.value(1).timeout(0, onTimeout: () => 'wrong'); }",
        "func main(): void { Timer.after(0, () => 1); }",
        "func main(): Future<Future<int>> { return Future.value(1); }"));

}  // namespace
}  // namespace vela::compiler
