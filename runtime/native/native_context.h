#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/type.h>

#include "base/execution_status.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_type.h"
#include "runtime/native/native_host_type.h"
#include "runtime/native/native_type.h"
#include "runtime/native/native_type_traits.h"
#include "runtime/value/host_object.h"
#include "runtime/value/value.h"
#include "vela/types.h"

namespace vela::runtime {

class Isolate;
class Future;
class Completer;
class CancellationToken;
class NativeExecution;

using NativeResult = std::expected<Value, vela::Error>;

template <NativeHostClass T>
struct NativeConstructed final {
  NativeConstructed(HostConstruction storage, T& value) noexcept
      : storage(std::move(storage)), value(&value) {}
  HostConstruction storage;
  T* value;
};

class NativeContext final {
 public:
  explicit NativeContext(Isolate& isolate) noexcept : isolate_(&isolate) {}
  ~NativeContext() = default;
  NativeContext(const NativeContext&) = delete;
  NativeContext& operator=(const NativeContext&) = delete;
  NativeContext(NativeContext&&) = delete;
  NativeContext& operator=(NativeContext&&) = delete;
  [[nodiscard]] Isolate& isolate() const noexcept { return *isolate_; }
  [[nodiscard]] HandleScope& handles() const noexcept;
  void ReleaseHandles() noexcept {
    if (handles_ != nullptr) {
      handles_storage_.reset();
      handles_ = nullptr;
    }
  }
  template <class T>
  [[nodiscard]] Result<vela::Type<T>> BindType() const {
    return vela::internal::TypeAccess::FromBinding<T>(
        BindTypeContract(internal::NativeTypeOf<T>()));
  }
  [[nodiscard]] NativeResult String(std::string_view text) const;
  template <class T>
  [[nodiscard]] Result<Local<vela::Future<T>>> FutureValue(const Future& future) const {
    auto value = TypedFutureValue(internal::NativeTypeOf<vela::Future<T>>(), future);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    return vela::internal::HandleAccess::Rebind<vela::Future<T>>(*value);
  }
  [[nodiscard]] Completer CreateCompleter() const;

  template <NativeHostClass T, class... Args>
  [[nodiscard]] std::expected<NativeConstructed<T>, vela::Error> Construct(
      Args&&... arguments) const {
    static_assert(std::is_nothrow_constructible_v<T, Args...>);
    static_assert(std::is_nothrow_destructible_v<T>);
    static_assert(sizeof(T) <= std::numeric_limits<std::uint32_t>::max());
    auto storage = PrepareHostConstruction(
        NativeHostType<T>(), sizeof(T), alignof(T),
        [](void* pointer) noexcept { std::destroy_at(static_cast<T*>(pointer)); });
    if (!storage) {
      return std::unexpected(std::move(storage.error()));
    }
    auto* value =
        std::construct_at(static_cast<T*>(storage->pointer()), std::forward<Args>(arguments)...);
    return NativeConstructed<T>(std::move(*storage), *value);
  }

  template <NativeHostClass T>
  [[nodiscard]] NativeResult Own(std::unique_ptr<T> value) const {
    static_assert(std::is_nothrow_destructible_v<T>);
    static_assert(!std::is_const_v<T>);
    return PublishHost(NativeNullableHostType<T>(),
                       HostObjectStorage(value.release(), [](void* pointer) noexcept {
                         std::default_delete<T>{}(static_cast<T*>(pointer));
                       }));
  }

  template <NativeHostClass T>
  [[nodiscard]] NativeResult Own(std::shared_ptr<T> value) const {
    static_assert(!std::is_const_v<T>);
    return PublishHost(NativeNullableHostType<T>(), HostObjectStorage(std::move(value)));
  }

  template <NativeHostClass T>
  [[nodiscard]] NativeResult Own(NativeConstructed<T> value) const {
    return PublishHostConstruction(std::move(value.storage));
  }

  template <NativeHostClass T>
  [[nodiscard]] std::expected<void, vela::Error> ReportExternalMemory(
      T& object,
      std::size_t extra_bytes) const {
    return SetHostMemory(NativeHostType<T>(), std::addressof(object), extra_bytes);
  }

 private:
  friend class NativeExecution;
  friend struct NativeMemoryLayout;
  NativeContext(Isolate& isolate, NativeExecution& execution) noexcept
      : isolate_(&isolate), execution_(&execution) {}
  [[nodiscard]] std::expected<HostType, ExecutionStatus> BindTypeContract(NativeType type) const;
  [[nodiscard]] Result<Local<vela::Value>> TypedFutureValue(NativeType type,
                                                            const Future& future) const;
  [[nodiscard]] std::expected<HostConstruction, vela::Error> PrepareHostConstruction(
      NativeType type,
      std::size_t size,
      std::size_t alignment,
      HostObjectStorage::Destructor destroy) const;
  [[nodiscard]] NativeResult PublishHostConstruction(HostConstruction construction) const;
  [[nodiscard]] NativeResult PublishHost(NativeType type, HostObjectStorage storage) const;
  [[nodiscard]] std::expected<void, vela::Error> SetHostMemory(NativeType type,
                                                               const void* object,
                                                               std::size_t extra_bytes) const;
  Isolate* isolate_;
  NativeExecution* execution_ = nullptr;
  mutable HandleScope* handles_ = nullptr;
  // optional 管理惰性作用域的生命周期；独立借用指针供生成代码检查，避免依赖标准库布局。
  mutable std::optional<HandleScope> handles_storage_;
};

// 同步宿主重入创建新的 NativeExecution 和 NativeContext；本上下文的句柄仅属于
// 当前宿主调用。存储由执行复用，标量调用不在每次适配时构造大块栈上作用域。
class NativeContextScope final {
 public:
  explicit NativeContextScope(NativeContext& context) noexcept : context_(&context) {}
  ~NativeContextScope() { context_->ReleaseHandles(); }
  NativeContextScope(const NativeContextScope&) = delete;
  NativeContextScope& operator=(const NativeContextScope&) = delete;
  NativeContextScope(NativeContextScope&&) = delete;
  NativeContextScope& operator=(NativeContextScope&&) = delete;

 private:
  NativeContext* context_;
};

}  // namespace vela::runtime
