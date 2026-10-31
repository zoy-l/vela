#pragma once

#include <cstdint>
#include <expected>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

[[nodiscard]] bool IsEqualValue(Value left, Value right) noexcept;
[[nodiscard]] std::expected<Value, ExecutionStatus> ConcatenateValues(Heap& heap,
                                                                      Value left,
                                                                      Value right);
[[nodiscard]] std::expected<bool, ExecutionStatus> CompareStrings(Value left, Value right);
[[nodiscard]] std::expected<Value, ExecutionStatus> ReadIndex(HeapObject* object,
                                                              std::int64_t index);
[[nodiscard]] std::expected<void, ExecutionStatus> WriteIndex(Heap& heap,
                                                              HeapObject* object,
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
