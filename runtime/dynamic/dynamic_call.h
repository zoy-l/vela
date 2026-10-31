#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "base/callable.h"
#include "base/execution_status.h"
#include "runtime/value/callable.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;

enum class DynamicCallStyle : std::uint8_t { Function, Method };

struct DynamicCallPlan final {
  Value value;
  // method 存在时 value 是接收者；否则 value 是待调用的函数值。
  std::optional<std::uint32_t> method = std::nullopt;
};

[[nodiscard]] std::expected<Value, ExecutionStatus> CreateDynamicCallPlan(
    Heap& heap,
    const DynamicCallPlan& plan);
[[nodiscard]] std::expected<DynamicCallPlan, ExecutionStatus> ReadDynamicCallPlan(Value value);

struct BoundArguments final {
  std::vector<std::uint32_t> source_indices;
  std::uint32_t skipped = 0;
  std::uint32_t rest_start = 0;
  std::uint32_t rest_count = 0;
};

[[nodiscard]] std::expected<BoundArguments, ExecutionStatus> BindDynamicArguments(
    const CallSignature& signature,
    const ResolvedCallable& callable,
    const HeapObject& arguments,
    const HeapObject& names,
    DynamicCallStyle style);
[[nodiscard]] std::expected<void, ExecutionStatus> CheckDynamicArguments(
    const CallSignature& signature,
    std::span<const TypeConstraint> types,
    const HeapObject& arguments,
    const BoundArguments& bound);
// 返回 native/VM 参数表示，包含 enum 解箱、缺省空值和 rest 数组分配。
[[nodiscard]] std::expected<Value, ExecutionStatus> ReadDynamicArgument(
    Heap& heap,
    const CallSignature& signature,
    std::span<const TypeConstraint> types,
    const HeapObject& arguments,
    const BoundArguments& bound,
    std::size_t index);

}  // namespace vela::runtime
