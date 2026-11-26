#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

enum class ArrayAccessError : std::uint8_t;

namespace internal {

// 仅供内部重排现有元素；不得写入新值或让借用逸出，否则必须使用 Heap::IntegerElements。
std::expected<std::span<std::int64_t>, ArrayAccessError> BorrowIntegerElements(
    HeapObject& array) noexcept;

inline constexpr std::uint32_t kInlineArrayCapacity = 4;

enum class IntegerArrayRange : std::uint8_t { Unknown, Nonnegative31 };
inline constexpr std::uint64_t kNonnegative31Mask = std::numeric_limits<std::int32_t>::max();

// 数组身份与 GC 链接保持稳定；只有 data 指向的连续元素缓冲区在扩容时迁移。
struct ArrayStorage final {
  std::byte* data = nullptr;
  std::uint64_t mutation = 0;
  std::uint32_t capacity = 0;
  // 只从已知窄域变为未知；可写宿主视图逸出后，后续写入不能重新建立此事实。
  IntegerArrayRange integer_range = IntegerArrayRange::Unknown;
};

static_assert(sizeof(ArrayStorage) % alignof(Value) == 0);

template <class Array>
inline auto& ArrayBuffer(Array& array) noexcept {
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

template <class Array>
inline auto ElementBytes(Array& array, const ArrayLayout& layout, std::size_t length) noexcept {
  return ArrayBytes(array, layout).first(layout.element_size() * length);
}

}  // namespace internal
}  // namespace vela::runtime
