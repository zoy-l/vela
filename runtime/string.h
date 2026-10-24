#pragma once

#include <span>
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
  auto operator=(const StringObject&) -> StringObject& = delete;
  StringObject(StringObject&&) = delete;
  auto operator=(StringObject&&) -> StringObject& = delete;

  [[nodiscard]] auto value() const noexcept -> std::string_view {
    const void* bytes = std::span(this, 1).last(0).data();
    return {static_cast<const char*>(bytes), header_.length};
  }

 private:
  friend class Heap;

  // 字节紧随对象头，长度复用数组头字段；字符串内容在发布后不可变。
  HeapObject header_;
};

static_assert(std::is_standard_layout_v<StringObject>);
static_assert(std::is_trivially_destructible_v<StringObject>);
static_assert(sizeof(StringObject) == sizeof(HeapObject));

}  // namespace vela::runtime
