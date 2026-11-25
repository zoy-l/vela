#pragma once

#include <cassert>
#include <cstddef>
#include <expected>
#include <utility>

#include "runtime/heap/heap.h"

#include "base/execution_status.h"
#include "runtime/native/host_callback.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela {
class HandleScope;
template <class T>
class Persistent;
}

namespace vela::runtime {

class Isolate;

// 公共 Persistent、错误载荷和 C 持久句柄共用的不可变根槽；所有操作属于 Isolate 线程。
class HostRoot final {
 public:
  HostRoot() noexcept = default;
  ~HostRoot() { Reset(); }
  HostRoot(const HostRoot& other) noexcept : storage_(other.storage_) { Retain(); }
  HostRoot& operator=(const HostRoot& other) noexcept {
    if (this != &other) {
      Reset();
      storage_ = other.storage_;
      Retain();
    }
    return *this;
  }
  HostRoot(HostRoot&& other) noexcept : storage_(std::exchange(other.storage_, nullptr)) {}
  HostRoot& operator=(HostRoot&& other) noexcept {
    if (this != &other) {
      Reset();
      storage_ = std::exchange(other.storage_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] static std::expected<HostRoot, ExecutionStatus> Create(Isolate& isolate,
                                                                       Value value) noexcept;
  [[nodiscard]] bool IsEmpty() const noexcept { return storage_ == nullptr; }
  [[nodiscard]] Isolate& isolate() const noexcept {
    assert(storage_ != nullptr);
    return *storage_->isolate;
  }
  [[nodiscard]] Value value() const noexcept {
    assert(storage_ != nullptr);
    assert(IsOwnerThread());
    return storage_->value;
  }
  void Reset() noexcept {
    assert(storage_ == nullptr || IsOwnerThread());
    auto* storage = std::exchange(storage_, nullptr);
    if (storage != nullptr && --storage->references == 0) {
      Destroy(*storage);
    }
  }

 private:
  friend class ::vela::HandleScope;
  template <class T>
  friend class ::vela::Persistent;
  struct Storage final {
    Storage(Isolate& owner, Value source) noexcept;
    Isolate* isolate;
    Value value;
    RootRange root;
    std::size_t references = 1;
  };
  static void Destroy(Storage& storage) noexcept;
  // 作用域共享根的所有权并借用稳定槽；只用于构造读句柄，不作为输出目标。
  [[nodiscard]] Value& slot() const noexcept { return storage_->value; }
  // 仅同类型单标量参数及结果的根附带描述；调用方的 Function 类型已确立这一契约。
  [[nodiscard]] const ::vela::internal::BoundScalarCallback& callback() const noexcept {
    return *StorageAfter<const ::vela::internal::BoundScalarCallback>(*storage_);
  }
  [[nodiscard]] bool IsOwnerThread() const noexcept;
  explicit HostRoot(Storage& storage) noexcept : storage_(&storage) {}
  void Retain() noexcept {
    if (storage_ != nullptr) {
      assert(IsOwnerThread());
      ++storage_->references;
    }
  }
  Storage* storage_ = nullptr;
};

}  // namespace vela::runtime
