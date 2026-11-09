#include "runtime/async/execution_context.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "runtime/async/future.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

constexpr ExecutionContext kProducer{.session = 1, .suite = 1, .scope = 1, .invocation = 1};
constexpr ExecutionContext kListener{.session = 1, .suite = 1, .scope = 2, .invocation = 2};
constexpr ExecutionContext kLater{.session = 1, .suite = 1, .scope = 3, .invocation = 3};
constexpr std::int64_t kAnswer = 42;

TEST(ExecutionContextTest, MicrotasksAndTimersRestoreTheirCreationContext) {
  FutureScheduler scheduler;
  std::vector<std::optional<ExecutionContext>> observed;
  scheduler.SetExecutionContext(kProducer);
  scheduler.PostMicrotask([&] { observed.push_back(scheduler.context()); });
  const auto timer = scheduler.After(std::chrono::milliseconds(0), [&] {
    observed.push_back(scheduler.context());
    scheduler.PostMicrotask([&] { observed.push_back(scheduler.context()); });
  });
  scheduler.SetExecutionContext(kLater);
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(observed,
            (std::vector<std::optional<ExecutionContext>>{kProducer, kProducer, kProducer}));
  EXPECT_EQ(scheduler.context(), kLater);
}

TEST(ExecutionContextTest, ListenersKeepRegistrationContextAndForwardOriginalErrorIdentity) {
  FutureScheduler scheduler;
  scheduler.SetExecutionContext(kProducer);
  Future source(&scheduler);
  Future forwarded(&scheduler);
  std::vector<std::optional<ExecutionContext>> observed;
  scheduler.SetExecutionContext(kListener);
  source.ObserveResult([&](const FutureResult& completion) {
    observed.push_back(scheduler.context());
    EXPECT_TRUE(forwarded.CompleteError(completion.value, completion.diagnostic));
  });
  scheduler.SetExecutionContext(kLater);
  source.ObserveResult([&](const FutureResult&) { observed.push_back(scheduler.context()); });
  ASSERT_TRUE(source.CompleteError(Value::Integer(42)));
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(observed, (std::vector<std::optional<ExecutionContext>>{kListener, kLater}));
  EXPECT_EQ(scheduler.context(), kLater);
  const auto original = source.error_diagnostic();
  const auto copied = forwarded.error_diagnostic();
  if (!original || !copied) {
    FAIL() << "completed failures require diagnostics";
  }
  EXPECT_EQ(original.value().origin.context, kLater);
  EXPECT_EQ(copied.value().origin.context, kLater);
  EXPECT_NE(original.value().origin.identity, 0U);
  EXPECT_EQ(original.value().origin.identity, copied.value().origin.identity);
}

TEST(ExecutionContextTest, HostCompletionUsesEndpointCreationContext) {
  FutureScheduler scheduler;
  scheduler.SetExecutionContext(kProducer);
  auto completer = scheduler.CreateHostCompleter();
  auto future = completer.future();
  const auto endpoint = std::move(completer).Detach();
  scheduler.SetExecutionContext(kLater);
  future.ObserveResult([](const FutureResult&) {});
  std::thread producer([endpoint] { EXPECT_TRUE(endpoint.CompleteError(Value::Integer(42))); });
  producer.join();
  ASSERT_EQ(scheduler.PumpUntil(future, FuturePumpMode::WaitForHost), FuturePumpResult::Completed);
  const auto diagnostic = future.error_diagnostic();
  if (!diagnostic) {
    FAIL() << "host failure requires a diagnostic";
  }
  EXPECT_EQ(diagnostic.value().origin.context, kProducer);
  EXPECT_EQ(scheduler.context(), kLater);
}

TEST(ExecutionContextTest, OrdinaryErrorsStayOutsideTestsWhenForwardedInsideATest) {
  FutureScheduler scheduler;
  auto source = Future::Error(Value::Integer(kAnswer), &scheduler);
  Future forwarded(&scheduler);
  scheduler.SetExecutionContext(kListener);
  source.ObserveResult([&](const FutureResult& completion) {
    EXPECT_TRUE(forwarded.CompleteError(completion.value, completion.diagnostic));
  });
  forwarded.ObserveResult([](const FutureResult&) {});
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  const auto diagnostic = forwarded.error_diagnostic();
  if (!diagnostic) {
    FAIL() << "forwarded failure requires a diagnostic";
  }
  EXPECT_TRUE(diagnostic.value().origin.captured);
  EXPECT_FALSE(diagnostic.value().origin.context.has_value());
}

}  // namespace
}  // namespace vela::runtime
