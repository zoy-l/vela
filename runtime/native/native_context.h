#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "runtime/heap/host_memory.h"
#include "runtime/native/native_host_type.h"
#include "runtime/native/native_type.h"
#include "runtime/value/host_object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Isolate;
class Future;
class Completer;
class CancellationToken;

enum class NativeErrorKind : std::uint8_t { Failure, TypeMismatch, OutOfMemory, InvalidUtf8 };

struct NativeError final {
  NativeErrorKind kind = NativeErrorKind::Failure;
  std::string message;
};

using NativeResult = std::expected<Value, NativeError>;

template <NativeHostClass T>
struct NativeConstructed final {
  HostMemory memory;
  std::unique_ptr<T> value;
};

class NativeContext final {
 public:
  explicit NativeContext(Isolate& isolate) noexcept : isolate_(&isolate) {}
  [[nodiscard]] Isolate& isolate() const noexcept { return *isolate_; }
  [[nodiscard]] NativeResult String(std::string_view text) const;
  [[nodiscard]] std::expected<Value, NativeError> FutureValue(const Future& future) const;
  [[nodiscard]] Completer CreateCompleter() const;
  [[nodiscard]] NativeResult CancellationValue(const CancellationToken& token) const;

  template <NativeHostClass T, class... Args>
  [[nodiscard]] std::expected<NativeConstructed<T>, NativeError> Construct(
      Args&&... arguments) const {
    static_assert(std::is_nothrow_constructible_v<T, Args...>);
    static_assert(std::is_nothrow_destructible_v<T>);
    auto memory = ReserveHostMemory(sizeof(T));
    if (!memory.IsReserved()) {
      return std::unexpected(NativeError{
          .kind = NativeErrorKind::OutOfMemory,
          .message = "out of memory reserving native object",
      });
    }
    std::unique_ptr<T> value(::new (std::nothrow) T(std::forward<Args>(arguments)...));
    if (value == nullptr) {
      return std::unexpected(NativeError{
          .kind = NativeErrorKind::OutOfMemory,
          .message = "out of memory constructing native object",
      });
    }
    return NativeConstructed<T>{.memory = std::move(memory), .value = std::move(value)};
  }

  template <NativeHostClass T>
  [[nodiscard]] NativeResult Own(std::unique_ptr<T> value) const {
    static_assert(std::is_nothrow_destructible_v<T>);
    static_assert(!std::is_const_v<T>);
    return PublishHost(kNativeNullableHostType<T>,
                       HostObjectStorage(value.release(), [](void* pointer) noexcept {
                         std::default_delete<T>{}(static_cast<T*>(pointer));
                       }));
  }

  template <NativeHostClass T>
  [[nodiscard]] NativeResult Own(std::shared_ptr<T> value) const {
    static_assert(!std::is_const_v<T>);
    return PublishHost(kNativeNullableHostType<T>, HostObjectStorage(std::move(value)));
  }

  template <NativeHostClass T>
  [[nodiscard]] NativeResult Own(NativeConstructed<T> value) const {
    return PublishHost(kNativeHostType<T>, HostObjectStorage(
                                               value.value.release(),
                                               [](void* pointer) noexcept {
                                                 std::default_delete<T>{}(static_cast<T*>(pointer));
                                               },
                                               std::move(value.memory)));
  }

  template <NativeHostClass T>
  [[nodiscard]] std::expected<void, NativeError> ReportExternalMemory(
      T& object,
      std::size_t extra_bytes) const {
    return SetHostMemory(kNativeHostType<T>, std::addressof(object), extra_bytes);
  }

 private:
  [[nodiscard]] HostMemory ReserveHostMemory(std::size_t bytes) const noexcept;
  [[nodiscard]] NativeResult PublishHost(NativeType type, HostObjectStorage storage) const;
  [[nodiscard]] std::expected<void, NativeError> SetHostMemory(NativeType type,
                                                               const void* object,
                                                               std::size_t extra_bytes) const;
  Isolate* isolate_;
};

}  // namespace vela::runtime
