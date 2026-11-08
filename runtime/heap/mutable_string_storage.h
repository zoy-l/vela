#pragma once

#include <cstddef>

namespace vela::runtime::internal {

// String builder 的字节数位于对象头；扩容只替换这个缓冲区，保留对象身份。
struct MutableStringStorage final {
  char* data = nullptr;
  std::size_t capacity = 0;
};

}  // namespace vela::runtime::internal
