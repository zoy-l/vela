#pragma once

#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;

}  // namespace vela::runtime

namespace vela::testing {

// 只检查运行时种类和内置标量；不调用 getter、toJson 或用户转换代码。
[[nodiscard]] std::expected<runtime::Value, ExecutionStatus> InspectTestValue(
    std::span<const runtime::Value> arguments,
    runtime::Heap& heap);

}  // namespace vela::testing
