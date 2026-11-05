#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <absl/cleanup/cleanup.h>
#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/execution_status.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/async/future.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"
#include "vela/async.h"
#include "vela/entry.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/types.h"

namespace vela::compiler {
namespace {

std::optional<Completion<String>>*& TextCompletionSink() {
  static thread_local std::optional<Completion<String>>* sink = nullptr;
  return sink;
}

Result<Local<Future<String>>> BeginText(runtime::NativeContext& context) noexcept {
  auto pair = CreateCompletion<String>(context.handles());
  if (!pair) {
    return std::unexpected(std::move(pair.error()));
  }
  *TextCompletionSink() = pair->completion;
  return pair->future;
}

TEST(HostAsyncLanguageTest, WorkerCompletesManagedPayloadAfterCallAndInstanceEnd) {
  runtime::Isolate isolate(1);
  std::optional<Completion<String>> endpoint;
  auto& sink = TextCompletionSink();
  sink = &endpoint;
  const absl::Cleanup clear_sink = [] { TextCompletionSink() = nullptr; };
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<BeginText>("beginText")));
  Persistent<Future<std::int64_t>> saved;
  {
    const auto compiled = test::CompileCleanSource(R"(
external func beginText(): Future<string>;
export async func answer(): Future<int> {
  const text = await beginText();
  return text == 'answer' ? 42 : 0;
}
)",
                                                   isolate);
    ASSERT_TRUE(compiled.has_value()) << compiled.error();
    const auto entry = Entry<Future<std::int64_t>()>::Bind(**compiled, "answer");
    ASSERT_TRUE(entry.has_value());
    HandleScope scope(isolate);
    const auto future = entry->Call(scope);
    ASSERT_TRUE(future.has_value());
    auto root = future->Persist();
    ASSERT_TRUE(root.has_value());
    saved = std::move(*root);
  }
  sink = nullptr;
  if (!endpoint) {
    FAIL() << "expected the host completion endpoint";
  }
  isolate.heap().Collect();
  std::optional<Result<void>> posted;
  std::thread worker(
      [completion = *endpoint, &posted] { posted = completion.Complete(std::string("answer")); });
  worker.join();
  if (!posted) {
    FAIL() << "expected a completion result from the worker";
  }
  ASSERT_TRUE(*posted);
  {
    HandleScope scope(isolate);
    const auto future = saved.ToLocal(scope);
    ASSERT_TRUE(future.has_value());
    saved.Reset();
    const auto result = future->Wait(scope, WaitMode::WaitForHost);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, 42);
  }
  endpoint.reset();
  isolate.heap().Collect();
  EXPECT_TRUE(isolate.ReloadBlockers().empty());
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(HostAsyncLanguageTest, HostTimersRetainScriptClosuresAndCancellationReleasesThem) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(R"(
let calls = 0;
export func callback(): () => void { return () => { calls++; }; }
export func count(): int { return calls; }
export func token(value: CancellationToken): CancellationToken { return value; }
export func cancel(value: Timer): void { value.cancel(); }
)",
                                                 isolate);
  ASSERT_TRUE(compiled.has_value()) << compiled.error();
  const auto callback = Entry<Function<void()>()>::Bind(**compiled, "callback");
  const auto count = Entry<std::int64_t()>::Bind(**compiled, "count");
  const auto cancel = Entry<void(Timer)>::Bind(**compiled, "cancel");
  const auto exchange = Entry<CancellationToken(CancellationToken)>::Bind(**compiled, "token");
  ASSERT_TRUE(callback.has_value());
  ASSERT_TRUE(count.has_value());
  ASSERT_TRUE(cancel.has_value());
  ASSERT_TRUE(exchange.has_value());
  HandleScope scope(isolate);
  const auto action = callback->Call(scope);
  ASSERT_TRUE(action.has_value());
  const auto token = CreateCancellationToken(scope);
  ASSERT_TRUE(token.has_value());
  const auto returned = exchange->Call(scope, *token);
  ASSERT_TRUE(returned.has_value());
  auto source = GetCancellationSource(*returned);
  source.Cancel();
  EXPECT_TRUE(IsCancelled(*token));
  const auto once = ScheduleTimer(scope, std::chrono::milliseconds(0), *action);
  ASSERT_TRUE(once.has_value());
  isolate.heap().Collect();
  ASSERT_EQ(isolate.scheduler().PumpOne(), runtime::FuturePumpStep::Executed);
  auto result = count->Call(scope);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1);
  const auto periodic =
      ScheduleTimer(scope, std::chrono::milliseconds(1), *action, TimerMode::Periodic);
  ASSERT_TRUE(periodic.has_value());
  ASSERT_EQ(isolate.scheduler().PumpOne(), runtime::FuturePumpStep::Executed);
  ASSERT_TRUE(cancel->Call(scope, *periodic));
  EXPECT_EQ(isolate.scheduler().PumpOne(), runtime::FuturePumpStep::Idle);
  result = count->Call(scope);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 2);
  const auto stopped = ScheduleTimer(scope, std::chrono::milliseconds(0), *action);
  ASSERT_TRUE(stopped.has_value());
  Cancel(*stopped);
  EXPECT_EQ(isolate.scheduler().PumpOne(), runtime::FuturePumpStep::Idle);
  isolate.heap().SetAllocationLimit(isolate.heap().allocated_bytes());
  const auto exhausted = ScheduleTimer(scope, std::chrono::milliseconds(0), *action);
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error().status, ExecutionStatus::OutOfMemory);
  EXPECT_EQ(isolate.scheduler().PumpOne(), runtime::FuturePumpStep::Idle);
}

}  // namespace
}  // namespace vela::compiler
