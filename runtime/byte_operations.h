#pragma once

#include <expected>
#include <span>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/value.h"

namespace vela::runtime {

class Heap;

// 参数数量由字节码验证；分配操作在安全点期间保根其输入，成功后由调用者立即发布结果。
[[nodiscard]] std::expected<Value, ExecutionStatus>
ExecuteByteBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap);

}  // namespace vela::runtime
