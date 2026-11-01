#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;

enum class TestInspection : std::uint8_t { Kind, Display };

// 只检查运行时种类和内置标量；不调用 getter、toJson 或用户转换代码。
[[nodiscard]] std::expected<Value, ExecutionStatus> InspectTestValue(
    std::span<const Value> arguments,
    Heap& heap);

}  // namespace vela::runtime
