#pragma once

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace vela {

// 操作编号范围为 [0, count)，窗口大小在编译期由同一份参数契约推导。
template <typename Operation>
consteval std::size_t MaximumOperationArity(Operation count,
                                            std::size_t (*arity)(Operation) noexcept) {
  std::size_t maximum = 0;
  for (std::underlying_type_t<Operation> index = 0; index < std::to_underlying(count); ++index) {
    maximum = std::max(maximum, arity(static_cast<Operation>(index)));
  }
  return maximum;
}

}  // namespace vela
