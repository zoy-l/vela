#pragma once

#include <cassert>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <vela/error.h>
#include <vela/types.h>
#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/heap/local_handle_stack.h"
#include "runtime/native/host_callback.h"
#include "runtime/value/value.h"

namespace vela {

namespace internal {
// 只按 Isolate 地址定位其 Heap；不读取或修改对象状态，避免句柄头反向包含绑定模板。
[[gnu::const]] runtime::Heap& HandleScopeHeap(runtime::Isolate& isolate) noexcept;
bool IsHandleScopeOwnerThread(runtime::Isolate& isolate) noexcept;
struct HandleAccess;
template <class T>
struct HostFunctionCall;

template <class T>
inline constexpr bool kIsScalarCallback =
    std::is_same_v<T, Function<std::int64_t(std::int64_t)>> ||
    std::is_same_v<T, Function<double(double)>> || std::is_same_v<T, Function<bool(bool)>>;
struct NoScalarCallback {};
template <class T>
using LocalCallback =
    std::conditional_t<kIsScalarCallback<T>, const BoundScalarCallback*, NoScalarCallback>;
}

template <class T>
class Local;
template <class T>
class Persistent;

class HandleScope final {
 public:
  [[clang::always_inline]] explicit HandleScope(runtime::Isolate& isolate) noexcept
      : isolate_(&isolate), storage_(&internal::HandleScopeHeap(isolate).local_handles()) {
    assert(internal::IsHandleScopeOwnerThread(isolate));
  }
  [[clang::always_inline]] ~HandleScope() {
    assert(internal::IsHandleScopeOwnerThread(*isolate_));
    if (borrowed_ != nullptr) {
      runtime::HostRoot::EndBorrow(*borrowed_, borrow_kind_);
    }
    storage_->Leave(segment_);
  }
  HandleScope(const HandleScope&) = delete;
  HandleScope& operator=(const HandleScope&) = delete;
  HandleScope(HandleScope&&) = delete;
  HandleScope& operator=(HandleScope&&) = delete;

  [[nodiscard]] runtime::Isolate& isolate() const noexcept { return *isolate_; }

 private:
  friend struct internal::HandleAccess;
  friend class ErrorPayload;
  template <class T>
  friend class Persistent;
  [[nodiscard]] std::expected<runtime::Value*, ExecutionStatus> Store(
      runtime::Value value) noexcept;
  [[nodiscard, clang::always_inline]] std::expected<runtime::Value*, ExecutionStatus> StoreRooted(
      runtime::Value value) noexcept {
    return storage_->StoreRooted(segment_, value);
  }
  template <class T>
  [[nodiscard]] std::expected<Local<T>, ExecutionStatus> Localize(
      const runtime::HostRoot& root) noexcept;

  runtime::Isolate* isolate_;
  runtime::LocalHandleStack* storage_;
  runtime::LocalHandleStack::Segment* segment_ = nullptr;
  // 首个本地化的不可变根直接借用其稳定槽，无需复制成 GC 槽。
  runtime::HostRoot::Storage* borrowed_ = nullptr;
  runtime::HostRoot::BorrowKind borrow_kind_ = runtime::HostRoot::BorrowKind::Primary;
};

template <class T>
class Local final {
 public:
  static_assert(
      internal::HostValueType<T>::kManaged &&
          !requires { typename internal::HostValueType<T>::Payload; },
      "Local requires a managed logical type; use scalars directly");
  [[nodiscard]] runtime::Isolate& isolate() const noexcept { return *isolate_; }
  [[nodiscard]] Result<Persistent<T>> Persist() const noexcept;
  [[nodiscard]] Local<Value> ToValue() const noexcept;
  [[nodiscard]] std::string_view Utf8() const noexcept
    requires std::is_same_v<T, String>;

  template <class U = T>
    requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
  [[nodiscard]] Result<HostValue<typename internal::ArrayType<U>::Element>> Get(
      HandleScope& scope,
      std::int64_t index) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
  [[nodiscard]] Result<void> Set(HandleScope& scope,
                                 std::int64_t index,
                                 HostValue<typename internal::ArrayType<U>::Element> value) const;
  [[nodiscard]] std::int64_t Size() const noexcept
    requires(internal::ArrayType<T>::kIsArray || internal::CollectionType<T>::kIsCollection);
  [[nodiscard]] std::int64_t Capacity() const noexcept
    requires internal::ArrayType<T>::kIsArray;
  [[nodiscard]] Result<void> Reserve(HandleScope& scope, std::int64_t capacity) const
    requires internal::ArrayType<T>::kIsArray;
  [[nodiscard]] Result<void> Clear(HandleScope& scope) const
    requires(internal::ArrayType<T>::kIsArray || internal::CollectionType<T>::kIsCollection);
  [[nodiscard]] Result<void> Reverse(HandleScope& scope) const
    requires internal::ArrayType<T>::kIsArray;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
  [[nodiscard]] Result<std::int64_t> Push(
      HandleScope& scope,
      HostValue<typename internal::ArrayType<U>::Element> value) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
  [[nodiscard]] Result<std::optional<HostValue<typename internal::ArrayType<U>::Element>>> Pop(
      HandleScope& scope) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
  [[nodiscard]] Result<std::optional<HostValue<typename internal::ArrayType<U>::Element>>> Shift(
      HandleScope& scope) const;
  [[nodiscard]] Result<Local<T>> Slice(HandleScope& scope,
                                       std::int64_t start,
                                       std::int64_t end) const
    requires internal::ArrayType<T>::kIsArray;
  [[nodiscard]] Result<Local<T>> Splice(HandleScope& scope,
                                        std::int64_t start,
                                        std::int64_t count,
                                        Local<T> items) const
    requires internal::ArrayType<T>::kIsArray;
  [[nodiscard]] Result<std::int64_t> PushMany(HandleScope& scope, Local<T> items) const
    requires internal::ArrayType<T>::kIsArray;
  [[nodiscard]] Result<std::int64_t> Unshift(HandleScope& scope, Local<T> items) const
    requires internal::ArrayType<T>::kIsArray;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
  [[nodiscard]] Result<void> Fill(HandleScope& scope,
                                  HostValue<typename internal::ArrayType<U>::Element> value,
                                  std::int64_t start,
                                  std::int64_t end) const;

  template <class U = T>
    requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsMap)
  [[nodiscard]] Result<std::optional<HostValue<typename internal::CollectionType<U>::Mapped>>> Get(
      HandleScope& scope,
      HostValue<typename internal::CollectionType<U>::Key> key) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsMap)
  [[nodiscard]] Result<void> Set(
      HandleScope& scope,
      HostValue<typename internal::CollectionType<U>::Key> key,
      HostValue<typename internal::CollectionType<U>::Mapped> value) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsCollection &&
             !internal::CollectionType<U>::kIsMap)
  [[nodiscard]] Result<Insertion> Add(
      HandleScope& scope,
      HostValue<typename internal::CollectionType<U>::Key> key) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsCollection)
  [[nodiscard]] Result<bool> Has(HandleScope& scope,
                                 HostValue<typename internal::CollectionType<U>::Key> key) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsCollection)
  [[nodiscard]] Result<Removal> Delete(
      HandleScope& scope,
      HostValue<typename internal::CollectionType<U>::Key> key) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsCollection)
  [[nodiscard]] Result<Local<Array<typename internal::CollectionType<U>::Key>>> Keys(
      HandleScope& scope) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsMap)
  [[nodiscard]] Result<Local<Array<typename internal::CollectionType<U>::Mapped>>> Values(
      HandleScope& scope) const;
  [[nodiscard]] Result<Local<T>> Union(HandleScope& scope, Local<T> other) const
    requires(internal::CollectionType<T>::kIsCollection && !internal::CollectionType<T>::kIsMap);
  [[nodiscard]] Result<Local<T>> Intersection(HandleScope& scope, Local<T> other) const
    requires(internal::CollectionType<T>::kIsCollection && !internal::CollectionType<T>::kIsMap);
  [[nodiscard]] Result<Local<T>> Difference(HandleScope& scope, Local<T> other) const
    requires(internal::CollectionType<T>::kIsCollection && !internal::CollectionType<T>::kIsMap);
  template <class U = T, class... Args>
    requires std::is_same_v<U, T>
  [[nodiscard]] Result<HostValue<typename internal::FunctionType<U>::Return>> Call(
      HandleScope& scope,
      Args&&... arguments) const;

  [[nodiscard]] runtime::FutureState State() const noexcept
    requires internal::AsyncType<T>::kIsFuture;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsFuture)
  [[nodiscard]] Result<HostValue<typename internal::AsyncType<U>::Payload>> Read(
      HandleScope& scope) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsFuture)
  [[nodiscard]] Result<HostValue<typename internal::AsyncType<U>::Payload>> Wait(
      HandleScope& scope,
      WaitMode mode = WaitMode::AvailableWork) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsCompleter)
  [[nodiscard]] Result<Local<Future<typename internal::AsyncType<U>::Payload>>> GetFuture(
      HandleScope& scope) const;
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsCompleter &&
             !std::is_void_v<typename internal::AsyncType<U>::Payload>)
  [[nodiscard]] Result<void> Complete(
      HandleScope& scope,
      HostValue<typename internal::AsyncType<U>::Payload> value) const;
  [[nodiscard]] Result<void> Complete(HandleScope& scope) const
    requires(internal::AsyncType<T>::kIsCompleter &&
             std::is_void_v<typename internal::AsyncType<T>::Payload>);
  template <class U = T>
    requires(std::is_same_v<U, T> && internal::AsyncType<U>::kIsCompleter)
  [[nodiscard]] Result<void> Adopt(
      HandleScope& scope,
      Local<Future<typename internal::AsyncType<U>::Payload>> future) const;
  [[nodiscard]] Result<void> Fail(HandleScope& scope, Error error) const
    requires internal::AsyncType<T>::kIsCompleter;

 private:
  friend struct internal::HandleAccess;
  friend class HandleScope;
  template <class U>
  friend struct internal::HostFunctionCall;
  template <class U>
  friend class Persistent;
  Local(runtime::Isolate& isolate,
        runtime::Value& value,
        internal::LocalCallback<T> callback = {}) noexcept
      : isolate_(&isolate), value_(&value), callback_(callback) {}
  // 槽仍由创建它的作用域覆盖；句柄访问仅借用 Isolate，不反向借用作用域对象。
  runtime::Isolate* isolate_;
  runtime::Value* value_;
#ifdef _MSC_VER
  [[msvc::no_unique_address]] internal::LocalCallback<T> callback_;
#else
  [[no_unique_address]] internal::LocalCallback<T> callback_;
#endif
};

template <class T>
class Persistent final {
 public:
  static_assert(
      internal::HostValueType<T>::kManaged &&
          !requires { typename internal::HostValueType<T>::Payload; },
      "Persistent requires a managed logical type; use scalars directly");
  Persistent() noexcept = default;
  ~Persistent() = default;
  Persistent(const Persistent&) noexcept = default;
  Persistent& operator=(const Persistent&) noexcept = default;
  Persistent(Persistent&& other) noexcept = default;
  Persistent& operator=(Persistent&& other) noexcept = default;
  [[nodiscard]] bool IsEmpty() const noexcept { return root_.IsEmpty(); }
  void Reset() noexcept { root_.Reset(); }
  [[nodiscard, clang::always_inline]] Result<Local<T>> ToLocal(HandleScope& scope) const noexcept {
    // 作用域独立保活根和回调描述，源 Persistent 随后可以立即释放。
    const auto localized = scope.Localize<T>(root_);
    if (!localized) [[unlikely]] {
      return std::unexpected(Error::FromStatus(localized.error()));
    }
    return *localized;
  }
  template <class U = T, class... Args>
    requires(std::is_same_v<U, T> && requires { typename internal::FunctionType<U>::Return; } &&
             !internal::HostValueType<typename internal::FunctionType<U>::Return>::kManaged)
  [[nodiscard]] Result<HostValue<typename internal::FunctionType<U>::Return>> Call(
      Args&&... arguments) const;

 private:
  friend class Local<T>;
  explicit Persistent(runtime::HostRoot root) noexcept : root_(std::move(root)) {}
  runtime::HostRoot root_;
};

template <class T>
Result<Persistent<T>> Local<T>::Persist() const noexcept {
  auto root = runtime::HostRoot::Create(isolate(), *value_);
  if (!root) {
    return std::unexpected(Error::FromStatus(root.error()));
  }
  return Persistent<T>(std::move(*root));
}

template <class T>
[[clang::always_inline]] std::expected<Local<T>, ExecutionStatus> HandleScope::Localize(
    const runtime::HostRoot& root) noexcept {
  auto* storage = root.storage_;
  if (storage == nullptr) [[unlikely]] {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  if (storage->isolate != isolate_) [[unlikely]] {
    return std::unexpected(ExecutionStatus::InvalidIsolate);
  }
  if (borrowed_ == nullptr) {
    // 首个本地化值总是借用稳定槽；其他作用域是否已借用只影响所有权记账。
    borrowed_ = storage;
    if (!storage->borrowed) [[likely]] {
      storage->borrowed = true;
    } else {
      // 作用域可非 LIFO 释放，不能依赖原借用作用域活得更久，因此共享一次引用。
      ++storage->references;
      borrow_kind_ = runtime::HostRoot::BorrowKind::Shared;
    }
    auto& slot = runtime::HostRoot::Slot(*storage);
    if constexpr (internal::kIsScalarCallback<T>) {
      return Local<T>(*isolate_, slot, &runtime::HostRoot::Callback(*storage));
    } else {
      return Local<T>(*isolate_, slot);
    }
  }
  const auto stored = StoreRooted(root.value());
  if (!stored) {
    return std::unexpected(stored.error());
  }
  return Local<T>(*isolate_, **stored);
}

inline Result<Local<Value>> ErrorPayload::ToLocal(HandleScope& scope) const noexcept {
  return scope.Localize<Value>(root_).transform_error(Error::FromStatus);
}

}  // namespace vela
