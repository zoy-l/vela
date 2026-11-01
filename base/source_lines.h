#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace vela {

using SourceLines = std::shared_ptr<const std::vector<std::size_t>>;

// 不可变源码及其所有位置消费者共享同一份行起点。
[[nodiscard]] SourceLines IndexSourceLines(std::string_view text);

}  // namespace vela
