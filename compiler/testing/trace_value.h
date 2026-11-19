#pragma once

#include <expected>
#include <span>
#include <vector>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;

}  // namespace vela::runtime

namespace vela::testing {

// 报告位置拥有文本与版本身份，复制 ByteArray 或退休机器码后仍可解析。
[[nodiscard]] std::expected<runtime::Value, ExecutionStatus> CaptureTestTrace(
    runtime::Heap& heap,
    std::span<const runtime::FutureTraceFrame> frames);
[[nodiscard]] std::expected<std::vector<runtime::FutureTraceFrame>, ExecutionStatus> ReadTestTrace(
    runtime::Value value);

}  // namespace vela::testing
