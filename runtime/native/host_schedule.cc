#include <vela/async.h>

#include <array>
#include <chrono>
#include <expected>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/async/future_composition.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela {

Result<Local<CancellationToken>> CancellationSource::ToLocal(HandleScope& scope) const {
  return internal::ReceiveHostValue<CancellationToken>(
      scope, [&](runtime::Value& output) -> Result<void> {
        auto& heap = scope.isolate().heap();
        auto* object = heap.AllocateCancellationToken(token_);
        if (object == nullptr) {
          return std::unexpected(Error::FromStatus(ExecutionStatus::OutOfMemory));
        }
        internal::StoreHostResult(heap, output, runtime::Value::Object(object));
        return {};
      });
}

Result<Local<CancellationToken>> CreateCancellationToken(HandleScope& scope) {
  return CancellationSource().ToLocal(scope);
}

CancellationSource GetCancellationSource(Local<CancellationToken> token) {
  return CancellationSource(
      *runtime::Heap::GetCancellationToken(*internal::HandleAccess::Read(token).AsObject()));
}

bool IsCancelled(Local<CancellationToken> token) noexcept {
  return runtime::Heap::GetCancellationToken(*internal::HandleAccess::Read(token).AsObject())
      ->IsCancelled();
}

void Cancel(Local<CancellationToken> token) noexcept {
  runtime::Heap::GetCancellationToken(*internal::HandleAccess::Read(token).AsObject())->Cancel();
}

void Cancel(Local<Timer> timer) noexcept {
  runtime::Heap::GetTimer(*internal::HandleAccess::Read(timer).AsObject())->Cancel();
}

Result<Local<Timer>> ScheduleTimer(HandleScope& scope,
                                   std::chrono::milliseconds delay,
                                   Local<Function<void()>> callback,
                                   TimerMode mode) {
  const auto callable = internal::EncodeHostValue<Function<void()>>(scope, callback);
  if (!callable) {
    return std::unexpected(callable.error());
  }
  auto& isolate = scope.isolate();
  if (const auto* parent = isolate.native_control().state().host_execution; parent != nullptr) {
    const auto checked = runtime::ResolveNativeCallable(parent->instance(), *callable);
    if (!checked) {
      return std::unexpected(Error::FromStatus(checked.error()));
    }
  }
  auto instance =
      runtime::NativeInstance::Borrow(*runtime::Heap::NativeEnvironment(*callable->AsObject()));
  const std::array arguments{runtime::Value::Integer(delay.count()), *callable};
  return internal::ReceiveHostValue<Timer>(scope, [&](runtime::Value& output) -> Result<void> {
    const auto result = runtime::ExecuteFutureOperation(
        isolate,
        {
            .operation = mode == TimerMode::Once ? AsyncOperation::TimerAfter
                                                 : AsyncOperation::TimerPeriodic,
            .values = arguments,
            .retained = instance.state(),
            .type_error = {},
            .timeout_error = {},
        },
        runtime::NativeFutureCallbacks(instance));
    if (!result) {
      return std::unexpected(Error::FromStatus(result.error()));
    }
    internal::StoreHostResult(isolate.heap(), output, *result);
    return {};
  });
}

}  // namespace vela
