#pragma once

#include <absl/container/inlined_vector.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

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
[[nodiscard]] DynamicCallPlan ReadDynamicCallPlan(Value value);

struct BoundArguments final {
  static constexpr std::size_t kInlineCapacity = 8;
  using SourceIndices = absl::InlinedVector<std::uint32_t, kInlineCapacity>;
  SourceIndices source_indices;
  std::uint32_t skipped = 0;
  std::uint32_t rest_start = 0;
  std::uint32_t rest_count = 0;
};

// names 为空表示内部数组回调的纯位置参数；显式动态调用提供与 arguments 等长的名称数组。
[[nodiscard]] std::expected<BoundArguments, ExecutionStatus> BindDynamicArguments(
    const CallSignature& signature,
    const ResolvedCallable& callable,
    const HeapObject& arguments,
    const HeapObject* names,
    DynamicCallStyle style);
// 返回原生调用参数表示，包含 enum 解箱、缺省空值和 rest 数组分配。
[[nodiscard]] std::expected<Value, ExecutionStatus> ReadDynamicArgument(
    Heap& heap,
    const CallSignature& signature,
    std::span<const TypeConstraint> types,
    const HeapObject& arguments,
    const BoundArguments& bound,
    std::size_t index);

}  // namespace vela::runtime
