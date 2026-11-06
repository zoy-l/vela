#pragma once

#include <expected>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>
#include "runtime/heap/host_root.h"
#include "runtime/value/value.h"
#include "vela/types.h"

namespace vela::internal {

// 仅运行时边界可以把受查的 runtime Value 放入公共句柄。
struct HandleAccess final {
  [[nodiscard]] static const runtime::HostRoot& ReadErrorPayload(const Error& error) noexcept {
    return error.payload.root_;
  }
  static void SetErrorPayload(Error& error, runtime::HostRoot root) noexcept {
    error.payload.root_ = std::move(root);
  }

  template <class T, class U>
  [[nodiscard]] static Local<T> Rebind(Local<U> value) noexcept {
    return Local<T>(*value.scope_, *value.value_);
  }

  template <class T>
  [[nodiscard]] static runtime::Value Read(Local<T> value) noexcept {
    return *value.value_;
  }

  [[nodiscard]] static runtime::Value& Slot(Local<Value> value) noexcept { return *value.value_; }
  [[nodiscard]] static HandleScope& Scope(Local<Value> value) noexcept { return *value.scope_; }

  // 参数或 C 边界已经登记的稳定槽由其原作用域覆盖，不重复申请根。
  template <class T>
  [[nodiscard]] static Local<T> Borrow(HandleScope& scope, runtime::Value& value) noexcept {
    return Local<T>(scope, value);
  }

  template <class T>
  [[nodiscard]] static Result<Local<T>> Root(HandleScope& scope, runtime::Value value) noexcept {
    const auto stored = scope.Store(value);
    if (!stored) {
      return std::unexpected(Error::FromStatus(stored.error()));
    }
    return Local<T>(scope, **stored);
  }
};

}  // namespace vela::internal
