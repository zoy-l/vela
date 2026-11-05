#pragma once

#include <cstdint>
#include <expected>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

// 分配 API 的空指针表示 OOM；在值边界统一转换为显式错误。
[[nodiscard]] std::expected<Value, ExecutionStatus> AllocatedValue(HeapObject* object) noexcept;
[[nodiscard]] std::expected<Value, ExecutionStatus> AllocatedValue(StringObject* string) noexcept;
// 调用方已确认对象属于枚举，序号由字段 0 的整型存储定义。
[[nodiscard]] std::int64_t EnumOrdinal(const HeapObject& object) noexcept;

// 私有 SDK 操作号必须属于声明为从零连续编号的枚举区间。
template <auto Last>
[[nodiscard]] std::expected<decltype(Last), ExecutionStatus> DecodeOperation(Value value) noexcept {
  if (!value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto number = value.AsInteger();
  if (number < 0 || number > static_cast<std::int64_t>(Last)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return static_cast<decltype(Last)>(number);
}

[[nodiscard]] bool IsEqualValue(Value left, Value right) noexcept;
[[nodiscard]] std::expected<Value, ExecutionStatus> ConcatenateValues(Heap& heap,
                                                                      Value left,
                                                                      Value right);
[[nodiscard]] std::expected<Value, ExecutionStatus> ReadIndex(HeapObject& object,
                                                              std::int64_t index);
[[nodiscard]] std::expected<void, ExecutionStatus> WriteIndex(Heap& heap,
                                                              HeapObject& object,
                                                              std::int64_t index,
                                                              Value value);
[[nodiscard]] std::expected<Value, ExecutionStatus> BindValueType(
    Value value,
    const TypeConstraint& type) noexcept;
[[nodiscard]] std::expected<Value, ExecutionStatus> CheckFuturePayload(
    Value value,
    const TypeConstraint& type) noexcept;

[[nodiscard]] std::expected<Value, ExecutionStatus> BoxDynamicValue(Heap& heap,
                                                                    Value value,
                                                                    const TypeConstraint& type);
// 调用方已验证值满足 type；仅将非空枚举装箱值转换为其存储表示。
[[nodiscard]] Value UnboxDynamicValue(Value value, const TypeConstraint& type) noexcept;

}  // namespace vela::runtime
