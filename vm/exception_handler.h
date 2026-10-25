#pragma once

#include <cstdint>

namespace vela::vm {

// 半开区间，按 begin 排序且互不重叠；target 必须指向 Catch 指令。
struct ExceptionHandler final {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  std::uint32_t target = 0;
};

}  // namespace vela::vm
