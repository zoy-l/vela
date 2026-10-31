#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "runtime/native/native_image.h"

namespace vela::runtime::test {

// 映像表只借用测试拥有的存储；调用方覆盖实例和捕获版本的整个使用期。
inline NativeText Text(std::string_view value) {
  return {.data = value.data(), .size = value.size()};
}

template <class T, std::size_t Size>
NativeTable<T> Table(const std::array<T, Size>& values) {
  return {.data = values.data(), .size = values.size()};
}

}  // namespace vela::runtime::test
