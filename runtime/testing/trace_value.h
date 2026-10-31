#pragma once

#include <expected>
#include <span>
#include <vector>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;

// 报告位置拥有文本与版本身份，复制 ByteArray 或退休机器码后仍可解析。
[[nodiscard]] std::expected<Value, ExecutionStatus> CaptureTestTrace(
    Heap& heap,
    std::span<const FutureTraceFrame> frames);
[[nodiscard]] std::expected<std::vector<FutureTraceFrame>, ExecutionStatus> ReadTestTrace(Value value);

}  // namespace vela::runtime
