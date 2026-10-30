#pragma once

#include <cstdint>
#include <expected>

#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

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

}  // namespace vela::runtime
