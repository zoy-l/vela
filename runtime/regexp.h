#pragma once

#include <expected>
#include <iosfwd>
#include <span>
#include <string>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

struct RegExpFailure final {
  ExecutionStatus status;
  std::string message;
};

// 调试输出只读取已保根的 RegExp/RegExpMatch，不执行模式或分配 GC 对象。
void WriteRegExp(std::ostream& output, HeapObject& object);

// 所有参数在操作期间保根；返回的对象由调用者在下一次分配前保根。
[[nodiscard]] std::expected<Value, RegExpFailure> ExecuteRegExpBuiltin(
    Builtin builtin,
    std::span<const Value> arguments,
    Heap& heap,
    const ArrayLayout* output_layout = nullptr);

}  // namespace vela::runtime
