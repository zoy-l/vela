#pragma once

#include <cstdint>
#include <span>

#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

enum class FieldState : std::uint8_t { Uninitialized, Initializing, Ready };

// 单次外部分配依次放置头、连续 Value 和初始化状态。弱链表不参与根追踪；
// Heap 在所属线程替换字段块，GC 扫描不跨步骤借用块内地址。
struct InstanceStorage final {
  HeapObject* object = nullptr;
  InstanceStorage* next = nullptr;
  InstanceStorage* previous = nullptr;
  std::uint32_t count = 0;

  [[nodiscard]] std::span<Value> values() noexcept { return {StorageAfter<Value>(*this), count}; }
  [[nodiscard]] std::span<const Value> values() const noexcept {
    return {StorageAfter<Value>(*this), count};
  }
  [[nodiscard]] std::span<FieldState> states() noexcept {
    void* storage = values().last(0).data();
    return {static_cast<FieldState*>(storage), count};
  }
};

}  // namespace vela::runtime
