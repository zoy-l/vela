#pragma once

#include <array>
#include <cstddef>
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
#include "runtime/value/value.h"

namespace vela {

namespace internal {
struct HandleAccess;
}

template <class T>
class Local;
template <class T>
class Persistent;

class HandleScope final {
 public:
  explicit HandleScope(runtime::Isolate& isolate) noexcept;
  ~HandleScope();
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
  struct Block;
  [[nodiscard]] std::expected<runtime::Value*, ExecutionStatus> Store(
      runtime::Value value) noexcept;
  template <class T>
  [[nodiscard]] Result<Local<T>> Localize(const runtime::HostRoot& root) noexcept;

  static constexpr std::size_t kInlineCapacity = 16;
  runtime::Isolate* isolate_;
  std::array<runtime::Value, kInlineCapacity> inline_values_;
  runtime::RootRange inline_roots_;
  std::size_t inline_count_ = 0;
  Block* blocks_ = nullptr;
};

template <class T>
class Local final {
 public:
  static_assert(
      internal::HostValueType<T>::kManaged &&
          !requires { typename internal::HostValueType<T>::Payload; },
      "Local requires a managed logical type; use scalars directly");
  [[nodiscard]] runtime::Isolate& isolate() const noexcept { return scope_->isolate(); }
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
  friend class Persistent;
  Local(HandleScope& scope, runtime::Value& value) noexcept : scope_(&scope), value_(&value) {}
  HandleScope* scope_;
  runtime::Value* value_;
};

template <class T>
class Persistent final {
 public:
  static_assert(
      internal::HostValueType<T>::kManaged &&
          !requires { typename internal::HostValueType<T>::Payload; },
      "Persistent requires a managed logical type; use scalars directly");
  Persistent() noexcept = default;
  [[nodiscard]] bool IsEmpty() const noexcept { return root_.IsEmpty(); }
  void Reset() noexcept { root_.Reset(); }
  [[nodiscard]] Result<Local<T>> ToLocal(HandleScope& scope) const noexcept {
    return scope.Localize<T>(root_);
  }

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
Result<Local<T>> HandleScope::Localize(const runtime::HostRoot& root) noexcept {
  if (root.IsEmpty()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
  }
  if (&root.isolate() != isolate_) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  const auto stored = Store(root.value());
  if (!stored) {
    return std::unexpected(Error::FromStatus(stored.error()));
  }
  return Local<T>(*this, **stored);
}

inline Result<Local<Value>> ErrorPayload::ToLocal(HandleScope& scope) const noexcept {
  return scope.Localize<Value>(root_);
}

}  // namespace vela
