#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/value.h"

namespace vela::runtime {

class Heap;
class ClassLayout;

inline constexpr std::uint32_t kDatePartsFieldCount = kUtcComponentCount;
inline constexpr std::int64_t kMinimumDateTimestamp = -62135596800000;
inline constexpr std::int64_t kMaximumDateTimestamp = 253402300799999;

struct DateFailure final {
  ExecutionStatus status;
  std::string_view message;
};

// 布局由标准声明绑定并在镜像加载时验证；日期运算不查询宿主时区或 locale。
[[nodiscard]] std::expected<Value, DateFailure> ExecuteDateBuiltin(
    Builtin builtin,
    std::span<const Value> arguments,
    Heap& heap,
    const ClassLayout* date_parts_layout);

}  // namespace vela::runtime
