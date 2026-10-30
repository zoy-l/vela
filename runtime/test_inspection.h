#pragma once

#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/value.h"

namespace vela::runtime {

class Heap;

// 只检查运行时种类和内置标量；不调用 getter、toJson 或用户转换代码。
[[nodiscard]] std::expected<Value, ExecutionStatus> InspectTestValue(
    std::span<const Value> arguments,
    Heap& heap);

}  // namespace vela::runtime
