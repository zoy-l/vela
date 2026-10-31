#pragma once

#include <expected>
#include <span>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;
class Isolate;

struct DynamicArrayInvocation final {
  Value callback;
  HeapObject* arguments = nullptr;
  HeapObject* names = nullptr;
};

[[nodiscard]] std::expected<Value, ExecutionStatus> PrepareDynamicArray(
    Isolate& isolate,
    std::span<const Value> input,
    std::span<ClassLayout* const> classes,
    const TypeSignature& callback_type);
[[nodiscard]] std::expected<DynamicArrayInvocation, ExecutionStatus> PrepareDynamicArrayInvocation(
    Heap& heap,
    std::span<const Value> input);
[[nodiscard]] std::expected<Value, ExecutionStatus> ReadDynamicArrayOperation(
    Builtin builtin,
    std::span<const Value> input);

}  // namespace vela::runtime
