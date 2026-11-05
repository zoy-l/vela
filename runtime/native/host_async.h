#pragma once

#include <expected>

#include <vela/error.h>
#include <vela/handles.h>

#include "runtime/async/future.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "vela/types.h"

namespace vela::internal {

[[nodiscard]] runtime::FutureState HostFutureState(runtime::Value future) noexcept;
[[nodiscard]] Result<void> ReadHostCompletion(HandleScope& scope,
                                              const runtime::FutureResult& result,
                                              runtime::Value& output);

[[nodiscard]] Result<void> CreateHostCompleter(HandleScope& scope,
                                               runtime::TypeConstraint type,
                                               runtime::Value& output);
[[nodiscard]] Result<runtime::FutureCompletionEndpoint>
CreateHostCompletion(HandleScope& scope, runtime::TypeConstraint type, runtime::Value& output);
[[nodiscard]] Result<void> ReadHostFuture(HandleScope& scope,
                                          runtime::Value future,
                                          runtime::Value& output);
[[nodiscard]] Result<void> WaitHostFuture(HandleScope& scope,
                                          runtime::Value future,
                                          WaitMode mode,
                                          runtime::Value& output);
[[nodiscard]] Result<void> GetHostFuture(HandleScope& scope,
                                         runtime::Value completer,
                                         runtime::Value& output);
[[nodiscard]] Result<void> CompleteHostFuture(runtime::HeapObject& completer, runtime::Value value);
[[nodiscard]] Result<void> AdoptHostFuture(runtime::HeapObject& completer, runtime::Value future);
[[nodiscard]] Result<void> FailHostFuture(HandleScope& scope,
                                          runtime::Value completer,
                                          Error error);
[[nodiscard]] Result<void> CompletionResult(
    std::expected<void, runtime::FutureCompletionError> result);

}  // namespace vela::internal
