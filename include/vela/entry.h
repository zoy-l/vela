#pragma once

#include <expected>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <vela/handles.h>
#include <vela/types.h>

#include "runtime/heap/heap.h"
#include "runtime/native/host_entry.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_type_traits.h"
#include "runtime/value/value.h"
#include "vela/error.h"

namespace vela {

template <class Signature>
class Entry;

template <class R, class... Args>
class Entry<R(Args...)> final {
 public:
  [[nodiscard]] static Result<Entry> Bind(runtime::NativeInstance& instance,
                                          std::string_view name) {
    return FromBinding(runtime::HostEntry::Bind(
        instance, name, runtime::internal::NativeTypeOf<Function<R(Args...)>>()));
  }
  [[nodiscard]] static Result<Entry> Bind(runtime::NativeDispatch& dispatch,
                                          std::string_view module,
                                          std::string_view name) {
    return FromBinding(runtime::HostEntry::Bind(
        dispatch, module, name, runtime::internal::NativeTypeOf<Function<R(Args...)>>()));
  }
  template <class... Actual>
  [[nodiscard]] Result<HostValue<R>> Call(HandleScope& scope, Actual&&... arguments) const {
    static_assert(
        std::is_same_v<std::tuple<std::remove_cvref_t<Actual>...>, std::tuple<HostValue<Args>...>>,
        "entry arguments must match the declared Vela types exactly");
    auto encoded =
        internal::EncodeHostArguments<Args...>(scope, std::forward<Actual>(arguments)...);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    return internal::ReceiveHostValue<R>(
        scope, [&](runtime::Value& output) { return entry_.Invoke(scope, *encoded, output); });
  }
  [[nodiscard]] Result<Entry> Capture() const { return FromBinding(entry_.Capture()); }
  [[nodiscard]] runtime::Isolate& isolate() const noexcept { return entry_.isolate(); }

 private:
  explicit Entry(runtime::HostEntry entry) : entry_(std::move(entry)) {}
  [[nodiscard]] static Result<Entry> FromBinding(Result<runtime::HostEntry> entry) {
    if (!entry) {
      return std::unexpected(std::move(entry.error()));
    }
    return Entry(std::move(*entry));
  }
  runtime::HostEntry entry_;
};

}  // namespace vela
