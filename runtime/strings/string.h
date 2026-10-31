#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "runtime/value/object.h"

namespace vela::runtime {

class StringObject final {
 private:
  struct AllocationToken {};

 public:
  explicit StringObject([[maybe_unused]] AllocationToken token) noexcept {}
  ~StringObject() = default;
  StringObject(const StringObject&) = delete;
  StringObject& operator=(const StringObject&) = delete;
  StringObject(StringObject&&) = delete;
  StringObject& operator=(StringObject&&) = delete;

  [[nodiscard]] std::string_view value() const noexcept {
    return {StorageAfter<char>(*this), header_.length};
  }
  [[nodiscard]] std::uint32_t length() const noexcept { return header_.string_length; }
  // 调用方已验证 index <= length()；允许末尾边界，用于切片。
  [[nodiscard]] std::size_t ByteOffset(std::uint32_t index) const noexcept;

 private:
  friend class Heap;

  // 字节发布后不可变；位置缓存仅在 Isolate 所属线程更新，无需 GC 写屏障。
  HeapObject header_;
};

static_assert(std::is_standard_layout_v<StringObject>);
static_assert(std::is_trivially_destructible_v<StringObject>);
static_assert(sizeof(StringObject) == sizeof(HeapObject));

inline constexpr std::size_t kMaximumStringLength =
    std::numeric_limits<std::uint32_t>::max() - sizeof(StringObject);

}  // namespace vela::runtime
