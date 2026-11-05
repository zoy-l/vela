#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/native/handle_access.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
class NativeExecution;
}

namespace vela::internal {

enum class HostPresence : std::uint8_t { Missing, Present };

[[nodiscard]] Error CaptureHostError(runtime::Isolate& isolate,
                                     ExecutionStatus status,
                                     runtime::Value payload,
                                     runtime::FutureDiagnostic diagnostic);
[[nodiscard]] Error CaptureHostError(runtime::NativeExecution& execution, ExecutionStatus status);

void StoreHostResult(runtime::Heap& heap, runtime::Value& output, runtime::Value value) noexcept;

[[nodiscard]] std::string_view HostStringUtf8(runtime::Value value) noexcept;
[[nodiscard]] Result<void> CreateHostString(HandleScope& scope,
                                            std::string_view text,
                                            runtime::Value& output);
[[nodiscard]] Result<void> CastHostValue(HandleScope& scope,
                                         runtime::Value value,
                                         runtime::TypeConstraint type,
                                         runtime::Value& output);
[[nodiscard]] Result<void> InvokeHostCallable(HandleScope& scope,
                                              runtime::Value callable,
                                              std::span<const runtime::Value> arguments,
                                              runtime::Value& output);

template <class T>
HostValue<T> DecodeHostScalar(runtime::Value value) {
  if constexpr (requires { typename HostValueType<T>::Payload; }) {
    if (value.IsNull()) {
      return std::nullopt;
    }
    return DecodeHostScalar<typename HostValueType<T>::Payload>(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    return value.AsBoolean();
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    return value.AsInteger();
  } else {
    static_assert(std::is_same_v<T, double>);
    return value.AsDouble();
  }
}

template <class T>
HostValue<T> DecodeHostLocal(Local<Value> value) {
  if constexpr (requires { typename HostValueType<T>::Payload; }) {
    if (HandleAccess::Read(value).IsNull()) {
      return std::nullopt;
    }
    return DecodeHostLocal<typename HostValueType<T>::Payload>(value);
  } else {
    return HandleAccess::Rebind<T>(value);
  }
}

template <class T>
Result<runtime::Value> EncodeHostValue(HandleScope& scope, const HostValue<T>& value) {
  if constexpr (requires { typename HostValueType<T>::Payload; }) {
    return value ? EncodeHostValue<typename HostValueType<T>::Payload>(scope, *value)
                 : Result<runtime::Value>(runtime::Value::Null());
  } else if constexpr (std::is_same_v<T, bool>) {
    return runtime::Value::Boolean(value);
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    return runtime::Value::Integer(value);
  } else if constexpr (std::is_same_v<T, double>) {
    return runtime::Value::Double(value);
  } else {
    if (&scope.isolate() != &value.isolate()) {
      return std::unexpected(
          Error{.status = ExecutionStatus::InvalidIsolate, .message = {}, .stack_trace = {}});
    }
    return HandleAccess::Read(value);
  }
}

template <class... Args>
Result<std::array<runtime::Value, sizeof...(Args)>> EncodeHostArguments(
    HandleScope& scope,
    const HostValue<Args>&... arguments) {
  std::array<runtime::Value, sizeof...(Args)> values{};
  if constexpr (sizeof...(Args) != 0) {
    std::optional<Error> failure;
    std::size_t index = 0;
    const auto encode = [&]<class T>(const HostValue<T>& value) {
      if (failure) {
        return;
      }
      auto encoded = EncodeHostValue<T>(scope, value);
      if (!encoded) {
        failure = std::move(encoded.error());
      } else {
        values[index++] = *encoded;
      }
    };
    (encode.template operator()<Args>(arguments), ...);
    if (failure) {
      return std::unexpected(std::move(*failure));
    }
  }
  return values;
}

// 托管结果直接写入目标作用域，生产者释放执行根前已建立最终根；标量不占用句柄槽。
template <class T, class Operation>
Result<HostValue<T>> ReceiveHostValue(HandleScope& scope, Operation&& operation) {
  if constexpr (HostValueType<T>::kManaged) {
    auto destination = HandleAccess::Root<Value>(scope, runtime::Value::Null());
    if (!destination) {
      return std::unexpected(std::move(destination.error()));
    }
    auto result = std::forward<Operation>(operation)(HandleAccess::Slot(*destination));
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
    return DecodeHostLocal<T>(*destination);
  } else {
    runtime::Value value;
    auto result = std::forward<Operation>(operation)(value);
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
    if constexpr (std::is_void_v<T>) {
      return {};
    } else {
      return DecodeHostScalar<T>(value);
    }
  }
}

// 缺失与存在的 null 各占一层，Map 和移除操作不会把二者合并。
template <class T, class Operation>
Result<std::optional<HostValue<T>>> ReceiveOptionalHostValue(HandleScope& scope,
                                                             Operation&& operation) {
  if constexpr (HostValueType<T>::kManaged) {
    auto destination = HandleAccess::Root<Value>(scope, runtime::Value::Null());
    if (!destination) {
      return std::unexpected(std::move(destination.error()));
    }
    auto result = std::forward<Operation>(operation)(HandleAccess::Slot(*destination));
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
    if (*result == HostPresence::Missing) {
      return std::nullopt;
    }
    return std::optional<HostValue<T>>(std::in_place, DecodeHostLocal<T>(*destination));
  } else {
    runtime::Value value;
    auto result = std::forward<Operation>(operation)(value);
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
    if (*result == HostPresence::Missing) {
      return std::nullopt;
    }
    return std::optional<HostValue<T>>(std::in_place, DecodeHostScalar<T>(value));
  }
}

template <class T>
struct HostFunctionCall;

template <class R, class... Args>
struct HostFunctionCall<Function<R(Args...)>> final {
  template <class... Actual>
  static Result<HostValue<R>> Call(HandleScope& scope,
                                   Local<Function<R(Args...)>> callable,
                                   Actual&&... arguments) {
    static_assert(
        std::is_same_v<std::tuple<std::remove_cvref_t<Actual>...>, std::tuple<HostValue<Args>...>>,
        "callback arguments must match the declared Vela types exactly");
    if (&scope.isolate() != &callable.isolate()) {
      return std::unexpected(
          Error{.status = ExecutionStatus::InvalidIsolate, .message = {}, .stack_trace = {}});
    }
    auto values = EncodeHostArguments<Args...>(scope, std::forward<Actual>(arguments)...);
    if (!values) {
      return std::unexpected(std::move(values.error()));
    }
    return ReceiveHostValue<R>(scope, [&](runtime::Value& output) {
      return InvokeHostCallable(scope, HandleAccess::Read(callable), *values, output);
    });
  }
};

}  // namespace vela::internal
