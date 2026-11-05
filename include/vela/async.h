#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <type_traits>
#include <utility>

#include <vela/handles.h>
#include <vela/type.h>
#include <vela/types.h>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_async.h"
#include "runtime/native/host_value.h"
#include "runtime/value/value.h"
#include "vela/error.h"

namespace vela {

enum class TimerMode : std::uint8_t { Once, Periodic };

using FutureObservation = runtime::FutureObservation;

template <class T, class Callback>
[[nodiscard]] Result<FutureObservation> ObserveFuture(HandleScope& scope,
                                                      Local<Future<T>> future,
                                                      Callback&& callback) {
  auto value = internal::EncodeHostValue<Future<T>>(scope, future);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  return internal::ObserveHostFuture(
      scope, *value,
      [action = std::forward<Callback>(callback)](HandleScope& handles,
                                                  const runtime::FutureResult& completion) mutable {
        auto result = internal::ReceiveHostValue<T>(handles, [&](runtime::Value& output) {
          return internal::ReadHostCompletion(handles, completion, output);
        });
        action(handles, std::move(result));
      });
}

// 普通句柄留在所属线程；这个能力只共享取消状态，可以跨线程保存和发出取消请求。
class CancellationSource final {
 public:
  CancellationSource() = default;
  explicit CancellationSource(runtime::CancellationToken token) : token_(std::move(token)) {}
  void Cancel() noexcept { token_.Cancel(); }
  [[nodiscard]] bool IsCancelled() const noexcept { return token_.IsCancelled(); }
  [[nodiscard]] Result<Local<CancellationToken>> ToLocal(HandleScope& scope) const;

 private:
  runtime::CancellationToken token_;
};

[[nodiscard]] Result<Local<CancellationToken>> CreateCancellationToken(HandleScope& scope);
[[nodiscard]] CancellationSource GetCancellationSource(Local<CancellationToken> token);
[[nodiscard]] bool IsCancelled(Local<CancellationToken> token) noexcept;
void Cancel(Local<CancellationToken> token) noexcept;
void Cancel(Local<Timer> timer) noexcept;
[[nodiscard]] Result<Local<Timer>> ScheduleTimer(HandleScope& scope,
                                                 std::chrono::milliseconds delay,
                                                 Local<Function<void()>> callback,
                                                 TimerMode mode = TimerMode::Once);

namespace internal {

template <class T>
runtime::Value EncodeCompletionValue(const HostValue<T>& value) {
  if constexpr (requires { typename HostValueType<T>::Payload; }) {
    return value ? EncodeCompletionValue<typename HostValueType<T>::Payload>(*value)
                 : runtime::Value::Null();
  } else if constexpr (std::is_same_v<T, bool>) {
    return runtime::Value::Boolean(value);
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    return runtime::Value::Integer(value);
  } else {
    static_assert(std::is_same_v<T, double>);
    return runtime::Value::Double(value);
  }
}

}  // namespace internal

// 端点仅持有现有关闭协议的控制块；不保存 Isolate、Local 或持久根。
template <class T>
class Completion final {
 public:
  explicit Completion(runtime::FutureCompletionEndpoint endpoint)
      : endpoint_(std::move(endpoint)) {}

  template <class U = T>
    requires(std::is_same_v<U, T> && !internal::HostValueType<U>::kManaged && !std::is_void_v<U>)
  [[nodiscard]] Result<void> Complete(HostValue<U> value) const {
    return internal::CompletionResult(
        endpoint_.Complete(internal::EncodeCompletionValue<U>(value)));
  }
  [[nodiscard]] Result<void> Complete() const
    requires std::is_void_v<T>
  {
    return internal::CompletionResult(endpoint_.Complete(runtime::Value::Null()));
  }
  [[nodiscard]] Result<void> Complete(std::string text) const
    requires std::is_same_v<T, String>
  {
    return internal::CompletionResult(endpoint_.CompleteText(std::move(text)));
  }
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::HostValueType<U>::kManaged)
  [[nodiscard]] Result<void> Complete(HandleScope& scope, HostValue<U> value) const {
    auto encoded = internal::EncodeHostValue<U>(scope, value);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    return internal::CompleteHostEndpoint(scope, endpoint_, *encoded);
  }
  [[nodiscard]] Result<void> Fail(std::string message) const {
    return internal::CompletionResult(endpoint_.CompleteError(
        runtime::Value::Null(), {
                                    .message = std::move(message),
                                    .stack_trace = {},
                                    .status = ExecutionStatus::NativeFailure,
                                    .origin = {},
                                }));
  }

 private:
  runtime::FutureCompletionEndpoint endpoint_;
};

template <class T>
struct CompletionPair final {
  Local<Future<T>> future;
  Completion<T> completion;
};

template <class T>
[[nodiscard]] Result<CompletionPair<T>> CreateCompletion(HandleScope& scope,
                                                         const Type<Future<T>>& type) {
  if (&scope.isolate() != &type.isolate()) {
    return std::unexpected(
        Error{.status = ExecutionStatus::InvalidIsolate, .message = {}, .stack_trace = {}});
  }
  auto destination = internal::HandleAccess::Root<Value>(scope, runtime::Value::Null());
  if (!destination) {
    return std::unexpected(std::move(destination.error()));
  }
  auto endpoint =
      internal::CreateHostCompletion(scope, internal::TypeAccess::Read(type).constraint(),
                                     internal::HandleAccess::Slot(*destination));
  if (!endpoint) {
    return std::unexpected(std::move(endpoint.error()));
  }
  return CompletionPair<T>{
      .future = internal::HandleAccess::Rebind<Future<T>>(*destination),
      .completion = Completion<T>(std::move(*endpoint)),
  };
}

template <class T>
[[nodiscard]] Result<CompletionPair<T>> CreateCompletion(HandleScope& scope) {
  auto type = Type<Future<T>>::Bind(scope.isolate());
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  return CreateCompletion(scope, *type);
}

template <class T>
[[nodiscard]] Result<Local<Completer<T>>> CreateCompleter(HandleScope& scope,
                                                          const Type<Completer<T>>& type) {
  if (&scope.isolate() != &type.isolate()) {
    return std::unexpected(
        Error{.status = ExecutionStatus::InvalidIsolate, .message = {}, .stack_trace = {}});
  }
  return internal::ReceiveHostValue<Completer<T>>(scope, [&](runtime::Value& output) {
    return internal::CreateHostCompleter(scope, internal::TypeAccess::Read(type).constraint(),
                                         output);
  });
}

template <class T>
[[nodiscard]] Result<Local<Completer<T>>> CreateCompleter(HandleScope& scope) {
  auto type = Type<Completer<T>>::Bind(scope.isolate());
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  return CreateCompleter(scope, *type);
}

template <class T>
runtime::FutureState Local<T>::State() const noexcept
  requires internal::AsyncType<T>::kIsFuture
{
  return internal::HostFutureState(internal::HandleAccess::Read(*this));
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsFuture)
Result<HostValue<typename internal::AsyncType<U>::Payload>> Local<T>::Read(
    HandleScope& scope) const {
  auto value = internal::EncodeHostValue<T>(scope, *this);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  return internal::ReceiveHostValue<typename internal::AsyncType<U>::Payload>(
      scope,
      [&](runtime::Value& output) { return internal::ReadHostFuture(scope, *value, output); });
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsFuture)
Result<HostValue<typename internal::AsyncType<U>::Payload>> Local<T>::Wait(HandleScope& scope,
                                                                           WaitMode mode) const {
  auto value = internal::EncodeHostValue<T>(scope, *this);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  return internal::ReceiveHostValue<typename internal::AsyncType<U>::Payload>(
      scope, [&](runtime::Value& output) {
        return internal::WaitHostFuture(scope, *value, mode, output);
      });
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsCompleter)
Result<Local<Future<typename internal::AsyncType<U>::Payload>>> Local<T>::GetFuture(
    HandleScope& scope) const {
  auto value = internal::EncodeHostValue<T>(scope, *this);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  return internal::ReceiveHostValue<Future<typename internal::AsyncType<U>::Payload>>(
      scope,
      [&](runtime::Value& output) { return internal::GetHostFuture(scope, *value, output); });
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsCompleter &&
           !std::is_void_v<typename internal::AsyncType<U>::Payload>)
Result<void> Local<T>::Complete(HandleScope& scope,
                                HostValue<typename internal::AsyncType<U>::Payload> value) const {
  auto values = internal::EncodeHostArguments<T, typename internal::AsyncType<U>::Payload>(
      scope, *this, value);
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }
  return internal::CompleteHostFuture(scope, *(*values)[0].AsObject(), (*values)[1]);
}

template <class T>
Result<void> Local<T>::Complete(HandleScope& scope) const
  requires(internal::AsyncType<T>::kIsCompleter &&
           std::is_void_v<typename internal::AsyncType<T>::Payload>)
{
  auto value = internal::EncodeHostValue<T>(scope, *this);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  return internal::CompleteHostFuture(scope, *value->AsObject(), runtime::Value::Null());
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsCompleter)
Result<void> Local<T>::Adopt(HandleScope& scope,
                             Local<Future<typename internal::AsyncType<U>::Payload>> future) const {
  auto values = internal::EncodeHostArguments<T, Future<typename internal::AsyncType<U>::Payload>>(
      scope, *this, future);
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }
  return internal::AdoptHostFuture(scope, *(*values)[0].AsObject(), (*values)[1]);
}

template <class T>
Result<void> Local<T>::Fail(HandleScope& scope, Error error) const
  requires internal::AsyncType<T>::kIsCompleter
{
  auto value = internal::EncodeHostValue<T>(scope, *this);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  return internal::FailHostFuture(scope, *value, std::move(error));
}

}  // namespace vela
