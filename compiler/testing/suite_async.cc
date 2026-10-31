#include "compiler/testing/result.h"
#include "compiler/testing/suite.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "runtime/execution_context.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/test_bridge.h"
#include "runtime/value.h"
#include "vm/diagnostic.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::testing {

namespace {

enum class PumpResult : std::uint8_t { Boundary, Idle };

PumpResult PumpToBoundary(runtime::FutureScheduler& scheduler,
                          const std::function<bool()>& pending,
                          std::optional<std::chrono::milliseconds> interval) {
  bool alarm = false;
  std::optional<runtime::Timer> timer;
  if (interval) {
    timer = scheduler.After(*interval, [&alarm] { alarm = true; });
  }
  auto result = PumpResult::Boundary;
  while (pending() && !alarm) {
    if (!scheduler.PumpOne(runtime::FuturePumpMode::WaitForHost)) {
      result = PumpResult::Idle;
      break;
    }
  }
  if (timer) {
    timer->Cancel();
  }
  return result;
}

}  // namespace

bool Suite::IsCleanupPhase(TestPhase phase) {
  return phase == TestPhase::DynamicTearDown || phase == TestPhase::TearDown ||
         phase == TestPhase::TearDownAll;
}

Suite::WaitStatus Suite::WaitUntil(Node& scope,
                                   std::optional<std::chrono::milliseconds> timeout,
                                   TestPhase phase,
                                   const std::function<bool()>& pending) {
  using Clock = runtime::FutureScheduler::Clock;
  auto& scheduler = isolate_->scheduler();
  const bool cleanup = IsCleanupPhase(phase);
  while (pending()) {
    if (!cleanup && options_.is_cancelled && options_.is_cancelled()) {
      scope.cancelled = true;
      return WaitStatus::Cancelled;
    }
    std::optional<std::chrono::milliseconds> interval;
    if (timeout) {
      interval = std::max(std::chrono::milliseconds(0),
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              scope.heartbeat + *timeout - Clock::now()));
    }
    if (options_.is_cancelled) {
      constexpr std::chrono::milliseconds kCancellationPoll{50};
      interval = std::min(interval.value_or(kCancellationPoll), kCancellationPoll);
    }
    if (PumpToBoundary(scheduler, pending, interval) == PumpResult::Idle) {
      return WaitStatus::Idle;
    }
    if (pending() && timeout && Clock::now() >= scope.heartbeat + *timeout) {
      return WaitStatus::TimedOut;
    }
  }
  return WaitStatus::Completed;
}

vm::ExecutionResult Suite::WaitFailure(WaitStatus status) const {
  auto& scheduler = isolate_->scheduler();
  auto failure_status = vm::ExecutionStatus::AsyncIdle;
  std::string message = "test Future has no producer";
  if (status == WaitStatus::Cancelled) {
    failure_status = vm::ExecutionStatus::ExecutionStopped;
    message = "test session cancelled";
  } else if (status == WaitStatus::TimedOut) {
    failure_status = vm::ExecutionStatus::Thrown;
    message = "TestTimeout: no test activity before the deadline";
  }
  return {
      .status = failure_status,
      .stack_trace = {},
      .message = std::move(message),
      .origin =
          {
              .context = scheduler.context(),
              .identity = scheduler.NewErrorIdentity(),
              .captured = true,
          },
  };
}

void Suite::AbandonExpectations(Node& scope) {
  for (auto& expectation : scope.expectations) {
    expectation.waiting = false;
    expectation.producer.reset();
  }
  for (const auto identity : scope.callback_expectations) {
    callbacks_.at(identity).state = CallbackExpectation::State::Abandoned;
  }
  scope.pending_expectations = 0;
  scope.pending_callbacks = 0;
}

void Suite::WaitExpectations(Node& scope, TestPhase phase, std::vector<TestError>& errors) {
  const auto status = WaitUntil(scope, scope.effective_timeout, phase, [&scope] {
    return scope.pending_expectations != 0 || scope.pending_callbacks != 0;
  });
  if (status != WaitStatus::Completed) {
    auto failure = WaitFailure(status);
    const std::vector<runtime::FutureTraceFrame>* trace = &scope.trace;
    for (const auto& expectation : scope.expectations) {
      if (expectation.waiting) {
        trace = &expectation.trace;
        break;
      }
    }
    for (const auto identity : scope.callback_expectations) {
      const auto& expectation = callbacks_.at(identity);
      if (expectation.state == CallbackExpectation::State::Pending) {
        trace = &expectation.trace;
        break;
      }
    }
    failure.stack_trace = vm::CopyExecutionTrace(*trace);
    RecordError(failure, phase, errors);
    AbandonExpectations(scope);
  }
}

vm::ExecutionResult Suite::Await(vm::ExecutionResult result,
                                 std::optional<std::chrono::milliseconds> timeout,
                                 Node& scope,
                                 TestPhase phase) {
  if (!result.Succeeded() || !result.value.IsObject()) {
    return result;
  }
  const auto* source = runtime::Heap::GetFuture(*result.value.AsObject());
  if (source == nullptr) {
    return result;
  }
  auto future = source->Rooted();
  struct Observation final {
    bool waiting = true;
    std::optional<std::size_t> retained;
  };
  auto observation = std::make_shared<Observation>();
  const auto identity = scope.id;
  future.ObserveResult([this, observation, identity, phase](const auto& completion) {
    if (!observation->waiting && completion.state == runtime::FutureState::Failed) {
      DeliverError(completion, identity, phase);
    }
    if (observation->retained) {
      scopes_.at(identity)->expectations.at(*observation->retained).future.reset();
    }
  });
  const auto status = WaitUntil(scope, timeout, phase, [&future] { return future.is_pending(); });
  observation->waiting = false;
  if (status != WaitStatus::Completed) {
    observation->retained = scope.expectations.size();
    scope.expectations.push_back(
        {.future = future.Rooted(), .trace = {}, .waiting = false, .producer = {}});
    AbandonExpectations(scope);
    return WaitFailure(status);
  }
  if (future.state() == runtime::FutureState::Failed) {
    return vm::FutureFailure(future.result());
  }
  result.value = future.result().value;
  return result;
}

std::expected<runtime::ExecutionContext, std::string> Suite::ExecutingContext() {
  const auto context = isolate_->scheduler().context();
  if (!context || state_ != State::Running) {
    return std::unexpected("asynchronous expectations require an executing test or hook");
  }
  auto* scope = scopes_.at(context->scope);
  if (scope->closed) {
    return std::unexpected("the test execution scope has already closed");
  }
  return *context;
}

std::expected<runtime::Value, std::string> Suite::TrackExpectation(
    runtime::Value value,
    std::vector<runtime::FutureTraceFrame> trace,
    ExpectationKind kind) {
  const auto executing = ExecutingContext();
  if (!executing) {
    return std::unexpected(executing.error());
  }
  auto* scope = scopes_.at(executing->scope);
  auto* source = value.IsObject() ? runtime::Heap::GetFuture(*value.AsObject()) : nullptr;
  if (source == nullptr) {
    return std::unexpected("asynchronous expectation must return a Future");
  }
  auto future = source->Rooted();
  const auto index = scope->expectations.size();
  scope->expectations.push_back({
      .future = future.Rooted(),
      .trace = std::move(trace),
      .producer = kind == ExpectationKind::CallObservation
                      ? std::make_shared<runtime::HostEventSource>(isolate_->scheduler())
                      : nullptr,
  });
  ++scope->pending_expectations;
  const auto identity = scope->id;
  const auto phase = phases_.at(executing->invocation);
  future.ObserveResult([this, identity, index, phase, kind](const auto& completion) {
    auto& owner = *scopes_.at(identity);
    auto& expectation = owner.expectations.at(index);
    expectation.producer.reset();
    if (expectation.waiting) {
      --owner.pending_expectations;
      if (kind == ExpectationKind::Assertion ||
          completion.state == runtime::FutureState::Succeeded) {
        owner.heartbeat = runtime::FutureScheduler::Clock::now();
      }
      expectation.waiting = false;
    }
    if (completion.state == runtime::FutureState::Failed) {
      DeliverError(completion, identity, phase);
    }
    expectation.future.reset();
  });
  return runtime::Value::Null();
}

std::expected<runtime::Value, std::string> Suite::RegisterCallback(
    std::span<const runtime::Value> arguments,
    std::vector<runtime::FutureTraceFrame> trace) {
  const auto executing = ExecutingContext();
  if (!executing) {
    return std::unexpected(executing.error());
  }
  const auto minimum = arguments.subspan(1).front();
  const auto maximum = arguments.subspan(2).front();
  if (!minimum.IsInteger() || !maximum.IsInteger() || minimum.AsInteger() < 0 ||
      maximum.AsInteger() < minimum.AsInteger()) {
    return std::unexpected("expectAsync requires 0 <= count <= max");
  }
  const auto context = *executing;
  const auto identity = next_id_++;
  callbacks_.emplace(identity,
                     CallbackExpectation{
                         .context = context,
                         .minimum = minimum.AsInteger(),
                         .maximum = maximum.AsInteger(),
                         .calls = 0,
                         .state = minimum.AsInteger() != 0 ? CallbackExpectation::State::Pending
                                                           : CallbackExpectation::State::Satisfied,
                         .trace = std::move(trace),
                     });
  auto& scope = *scopes_.at(context.scope);
  scope.callback_expectations.push_back(identity);
  if (minimum.AsInteger() != 0) {
    ++scope.pending_callbacks;
  }
  return runtime::Value::Integer(static_cast<std::int64_t>(identity));
}

std::expected<runtime::Value, std::string> Suite::EnterCallback(runtime::Value identity) {
  const auto found = FindCallback(identity);
  if (!found) {
    return std::unexpected(found.error());
  }
  auto& scheduler = isolate_->scheduler();
  callback_contexts_.push_back(scheduler.context());
  scheduler.SetExecutionContext((*found)->context);
  return runtime::Value::Integer(static_cast<std::int64_t>(callback_contexts_.size()));
}

std::expected<Suite::CallbackExpectation*, std::string> Suite::FindCallback(
    runtime::Value identity) {
  if (!identity.IsInteger()) {
    return std::unexpected("invalid callback expectation identity");
  }
  const auto found = callbacks_.find(static_cast<std::uint64_t>(identity.AsInteger()));
  if (found == callbacks_.end()) {
    return std::unexpected("unknown callback expectation identity");
  }
  return &found->second;
}

std::expected<runtime::Value, std::string> Suite::ExitCallback(runtime::Value ticket) {
  if (!ticket.IsInteger() || ticket.AsInteger() <= 0 ||
      static_cast<std::uint64_t>(ticket.AsInteger()) != callback_contexts_.size()) {
    return std::unexpected("callback execution scopes must close in stack order");
  }
  isolate_->scheduler().SetExecutionContext(callback_contexts_.back());
  callback_contexts_.pop_back();
  return runtime::Value::Null();
}

std::expected<runtime::Value, std::string> Suite::CheckCallback(runtime::Value identity) {
  const auto found = FindCallback(identity);
  if (!found) {
    return std::unexpected(found.error());
  }
  auto& expectation = **found;
  if (expectation.calls == expectation.maximum) {
    return std::unexpected("expectAsync callback called more than max times");
  }
  ++expectation.calls;
  auto& scope = *scopes_.at(expectation.context.scope);
  if (expectation.state != CallbackExpectation::State::Abandoned) {
    scope.heartbeat = runtime::FutureScheduler::Clock::now();
    if (expectation.state == CallbackExpectation::State::Pending &&
        expectation.calls == expectation.minimum) {
      --scope.pending_callbacks;
      expectation.state = CallbackExpectation::State::Satisfied;
    }
  }
  return runtime::Value::Null();
}

std::expected<runtime::Value, std::string> Suite::Control(
    runtime::TestControl operation,
    std::span<const runtime::Value> arguments,
    std::vector<runtime::FutureTraceFrame> trace) {
  switch (operation) {
    case runtime::TestControl::TrackExpectation:
      return TrackExpectation(arguments.front(), std::move(trace), ExpectationKind::Assertion);
    case runtime::TestControl::RegisterCallback:
      return RegisterCallback(arguments, std::move(trace));
    case runtime::TestControl::EnterCallback:
      return EnterCallback(arguments.front());
    case runtime::TestControl::ExitCallback:
      return ExitCallback(arguments.front());
    case runtime::TestControl::CheckCallback:
      return CheckCallback(arguments.front());
    case runtime::TestControl::CurrentTestScope:
      return CurrentTestScope();
    case runtime::TestControl::RegisterFinalizer:
      return RegisterFinalizer(arguments.front(), std::move(trace));
    case runtime::TestControl::ReleaseFinalizer:
      return ReleaseFinalizer(arguments.front());
    case runtime::TestControl::TrackCallWait:
      return TrackExpectation(arguments.front(), std::move(trace),
                              ExpectationKind::CallObservation);
  }
  std::unreachable();
}

runtime::Value Suite::CurrentTestScope() const {
  if (state_ != State::Running) {
    return runtime::Value::Integer(0);
  }
  const auto identity = CurrentTestId();
  if (!identity || scopes_.at(*identity)->closed) {
    return runtime::Value::Integer(0);
  }
  return runtime::Value::Integer(static_cast<std::int64_t>(*identity));
}

std::expected<runtime::Value, std::string> Suite::RegisterFinalizer(
    runtime::Value callback,
    std::vector<runtime::FutureTraceFrame> trace) {
  const auto identity = CurrentTestScope().AsInteger();
  if (identity == 0) {
    return std::unexpected("SDK finalizers require an executing test scope");
  }
  auto& scope = *scopes_.at(static_cast<std::uint64_t>(identity));
  const auto registration = next_id_++;
  scope.finalizers.push_back({
      .id = registration,
      .value = runtime::RootedValue(*isolate_, callback),
      .trace = std::move(trace),
  });
  return runtime::Value::Integer(static_cast<std::int64_t>(registration));
}

std::expected<runtime::Value, std::string> Suite::ReleaseFinalizer(runtime::Value identity) {
  const auto scope_id = CurrentTestScope().AsInteger();
  if (scope_id == 0 || !identity.IsInteger()) {
    return std::unexpected("SDK finalizer release requires its executing test scope");
  }
  auto& scope = *scopes_.at(static_cast<std::uint64_t>(scope_id));
  std::erase_if(scope.finalizers, [identity](const Callback& callback) {
    return std::cmp_equal(callback.id, identity.AsInteger());
  });
  return runtime::Value::Null();
}

void Suite::DeliverError(const runtime::FutureResult& completion,
                         std::uint64_t identity,
                         TestPhase phase) {
  auto& scope = *scopes_.at(identity);
  const auto failure = vm::FutureFailure(completion);
  if (scope.result) {
    auto& result = result_.tests.at(*scope.result);
    RecordError(failure, phase, result.errors);
    result.status = TestStatus::Failed;
  } else if (scope.closed || phase == TestPhase::Registration) {
    RecordError(failure, phase, result_.errors);
  } else {
    RecordError(failure, phase, scope.errors);
  }
}

}  // namespace vela::testing
