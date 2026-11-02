#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include "base/execution_status.h"

namespace vela::runtime {

class Heap;

void AppendJSONKey(std::string& path, std::string_view key);
// 只定位权威解析器已检查前缀中的结构位置，不独立判断 JSON 是否有效。
[[nodiscard]] std::expected<std::string, ExecutionStatus> JSONPathAtOffset(std::string_view source,
                                                                           std::size_t offset,
                                                                           Heap& heap);

}  // namespace vela::runtime
