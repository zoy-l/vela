#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "runtime/heap/field_storage.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

enum class FieldState : std::uint8_t { Uninitialized, Initializing, Ready };

// 单次外部分配依次放置头、字段载荷、标签和初始化状态。弱链表不参与根追踪；
// Heap 在所属线程替换字段块，GC 扫描不跨步骤借用块内地址。
struct InstanceStorage final {
  HeapObject* object = nullptr;
  InstanceStorage* next = nullptr;
  InstanceStorage* previous = nullptr;
  std::uint32_t count = 0;
  // Initializing 仍计入未就绪数；零值允许整个字段块跳过逐字段初始化检查。
  std::uint32_t unready = 0;

  [[nodiscard]] std::span<std::byte> fields() noexcept {
    return {StorageAfter<std::byte>(*this), FieldStorage::Size(count)};
  }
  [[nodiscard]] std::span<const std::byte> fields() const noexcept {
    return {StorageAfter<std::byte>(*this), FieldStorage::Size(count)};
  }
  [[nodiscard]] std::span<FieldState> states() noexcept {
    void* storage = fields().last(0).data();
    return {static_cast<FieldState*>(storage), count};
  }
  void SetFieldState(std::uint32_t index, FieldState state) noexcept {
    auto& current = states().subspan(index).front();
    unready += static_cast<std::uint32_t>(state != FieldState::Ready);
    unready -= static_cast<std::uint32_t>(current != FieldState::Ready);
    current = state;
  }
};

}  // namespace vela::runtime
