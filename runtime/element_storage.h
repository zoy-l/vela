#pragma once

#include <cstddef>
#include <span>

#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

// 调用者保证存储对齐、索引有效及写入值满足布局类型；这里仅负责连续存储的表示。
[[nodiscard]] auto LoadElementStorage(const ArrayLayout& layout,
                                      std::span<const std::byte> storage,
                                      std::size_t index) noexcept -> Value;
void StoreElementStorage(const ArrayLayout& layout,
                         std::span<std::byte> storage,
                         std::size_t index,
                         Value value) noexcept;

}  // namespace vela::runtime
