#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "runtime/object.h"

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

 private:
  friend class Heap;

  // 字节紧随对象头，长度复用数组头字段；字符串内容在发布后不可变。
  HeapObject header_;
};

static_assert(std::is_standard_layout_v<StringObject>);
static_assert(std::is_trivially_destructible_v<StringObject>);
static_assert(sizeof(StringObject) == sizeof(HeapObject));

inline constexpr std::size_t kMaximumStringLength =
    std::numeric_limits<std::uint32_t>::max() - sizeof(StringObject);

}  // namespace vela::runtime
