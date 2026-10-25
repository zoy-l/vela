#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "base/execution_status.h"
#include "runtime/object.h"

namespace vela::runtime {

// 只在一次执行内解释；函数编号属于保持存活的 Program，不保存借用指针。
struct TracePosition final {
  std::uint32_t function = 0;
  std::uint32_t instruction = 0;
};

// 隐藏的异常包络不暴露给脚本。payload 是 catch 可见的强类型类实例。
// 此数据紧随 HeapObject；trace 与消息紧随数据本体，均计入一次堆分配。
struct ExceptionData final {
  HeapObject* payload = nullptr;
  ExecutionStatus status = ExecutionStatus::Thrown;
  std::uint32_t trace_count = 0;
  std::uint32_t message_length = 0;

  [[nodiscard]] std::span<const TracePosition> trace() const noexcept;
  [[nodiscard]] std::string_view message() const noexcept;
  [[nodiscard]] static ExceptionData& From(HeapObject& object) noexcept;
};

}  // namespace vela::runtime
