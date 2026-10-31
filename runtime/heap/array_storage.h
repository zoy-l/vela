#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime::internal {

inline constexpr std::uint32_t kInlineArrayCapacity = 4;

// 数组身份与 GC 链接保持稳定；只有 data 指向的连续元素缓冲区在扩容时迁移。
struct ArrayStorage final {
  std::byte* data = nullptr;
  std::uint64_t mutation = 0;
  std::uint32_t capacity = 0;
};

static_assert(sizeof(ArrayStorage) % alignof(Value) == 0);

inline ArrayStorage& ArrayBuffer(HeapObject& array) noexcept {
  return *StorageAfter<ArrayStorage>(array);
}

inline const ArrayStorage& ArrayBuffer(const HeapObject& array) noexcept {
  return *StorageAfter<ArrayStorage>(array);
}

inline std::byte* InlineArrayData(ArrayStorage& buffer) noexcept {
  return StorageAfter<std::byte>(buffer);
}

inline std::span<std::byte> ArrayBytes(HeapObject& array, const ArrayLayout& layout) noexcept {
  const auto& buffer = ArrayBuffer(array);
  return {buffer.data, static_cast<std::size_t>(buffer.capacity) * layout.element_size()};
}

inline std::span<const std::byte> ArrayBytes(const HeapObject& array,
                                             const ArrayLayout& layout) noexcept {
  const auto& buffer = ArrayBuffer(array);
  return {buffer.data, static_cast<std::size_t>(buffer.capacity) * layout.element_size()};
}

}  // namespace vela::runtime::internal
