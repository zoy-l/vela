#pragma once

#include <span>
#include <vector>

#include "runtime/async/future.h"
#include "vm/debug_info.h"

namespace vela::vm {

struct ExecutionResult;

// 两种帧都拥有文本；转换不保留 Program 或原生代码资源的借用。
[[nodiscard]] std::vector<runtime::FutureTraceFrame> CopyFutureTrace(
    std::span<const StackFrame> trace);
[[nodiscard]] std::vector<StackFrame> CopyExecutionTrace(
    std::span<const runtime::FutureTraceFrame> trace);
[[nodiscard]] ExecutionResult FutureFailure(const runtime::FutureResult& completion);

}  // namespace vela::vm
