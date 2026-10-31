#include "runtime/dynamic/dynamic_call.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include "base/callable.h"
#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"
#include "runtime/value/callable.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {

std::expected<Value, ExecutionStatus> CreateDynamicCallPlan(Heap& heap,
                                                            const DynamicCallPlan& plan) {
  const RootRange root(heap, &plan.value, 1, RootOwner::Runtime);
  auto* storage = heap.AllocateValueSlots(2);
  if (storage == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(*storage, 0, plan.value);
  if (plan.method) {
    heap.SetValueSlot(*storage, 1, Value::Integer(*plan.method));
  }
  return Value::Object(storage);
}

std::expected<DynamicCallPlan, ExecutionStatus> ReadDynamicCallPlan(Value value) {
  if (!value.IsObject() || Heap::KindOf(*value.AsObject()) != ObjectKind::ValueSlots) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto storage = Heap::ValueSlots(*value.AsObject());
  if (storage.size() != 2) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto method = storage.subspan(1).front();
  if (!method.IsNull() && !method.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return DynamicCallPlan{
      .value = storage.front(),
      .method = method.IsNull() ? std::nullopt
                                : std::optional(static_cast<std::uint32_t>(method.AsInteger())),
  };
}

namespace {

constexpr auto kUnassigned = std::numeric_limits<std::uint32_t>::max();

struct ArgumentBinding final {
  const CallSignature* signature = nullptr;
  BoundArguments* bound = nullptr;
  DynamicCallStyle style = DynamicCallStyle::Function;
  std::uint32_t positional = 0;
  bool named = false;

  std::expected<void, ExecutionStatus> Bind(std::string_view name, std::uint32_t index) {
    const auto& call = *signature;
    auto& output = *bound;
    const auto& parameters = call.parameters;
    const auto skipped = output.skipped;
    std::optional<std::uint32_t> parameter;
    if (name.empty() && !named) {
      if (!parameters.empty() && parameters.back().rest &&
          positional + skipped >= parameters.size() - 1) {
        if (output.rest_count == 0) {
          output.rest_start = index;
        }
        ++output.rest_count;
        return {};
      }
      parameter = positional++ + skipped;
    } else if (!name.empty() && style == DynamicCallStyle::Method) {
      named = true;
      parameter = call.ParameterIndex(name);
    }
    if (!parameter || *parameter < skipped || *parameter >= parameters.size()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    auto& source = output.source_indices.at(*parameter - skipped);
    if (source != kUnassigned || parameters.at(*parameter).rest) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    source = index;
    return {};
  }
};

std::expected<Value, ExecutionStatus> PackRest(Heap& heap,
                                               const HeapObject& arguments,
                                               const BoundArguments& bound,
                                               const ArrayLayout& type) {
  auto* array = heap.AllocateArray(type, bound.rest_count);
  if (array == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  for (std::uint32_t item = 0; item < bound.rest_count; ++item) {
    const auto value = *Heap::GetElement(arguments, bound.rest_start + item);
    std::ignore = heap.SetElement(*array, item, UnboxDynamicValue(value, type.element));
  }
  return Value::Object(array);
}

}  // namespace

std::expected<BoundArguments, ExecutionStatus> BindDynamicArguments(
    const CallSignature& signature,
    const ResolvedCallable& callable,
    const HeapObject& arguments,
    const HeapObject& names,
    DynamicCallStyle style) {
  const auto count = Heap::ArrayLength(arguments);
  const auto names_count = Heap::ArrayLength(names);
  const auto hidden = callable.implicit ? 1U : 0U;
  if (!count || !names_count || *count != *names_count || hidden < signature.hidden_parameters) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto skipped = hidden - signature.hidden_parameters;
  const auto& parameters = signature.parameters;
  if (parameters.size() < skipped) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  BoundArguments bound{
      .source_indices = std::vector<std::uint32_t>(parameters.size() - skipped, kUnassigned),
      .skipped = skipped,
      .rest_start = *count,
  };
  ArgumentBinding binding{.signature = &signature, .bound = &bound, .style = style};
  for (std::uint32_t index = 0; index < *count; ++index) {
    const auto name = *Heap::GetElement(names, index);
    if (!name.IsString()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto result = binding.Bind(name.AsString()->value(), index);
    if (!result) {
      return std::unexpected(result.error());
    }
  }
  for (std::size_t index = 0; index < bound.source_indices.size(); ++index) {
    const auto& parameter = parameters.at(skipped + index);
    if (bound.source_indices.at(index) == kUnassigned && !parameter.optional && !parameter.rest) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  return bound;
}

std::expected<void, ExecutionStatus> CheckDynamicArguments(const CallSignature& signature,
                                                           std::span<const TypeConstraint> types,
                                                           const HeapObject& arguments,
                                                           const BoundArguments& bound) {
  for (std::size_t index = 0; index < bound.source_indices.size(); ++index) {
    const auto& parameter = signature.parameters.at(bound.skipped + index);
    const auto& type = types.subspan(parameter.type).front();
    if (parameter.rest) {
      for (std::uint32_t item = 0; item < bound.rest_count; ++item) {
        const auto value = *Heap::GetElement(arguments, bound.rest_start + item);
        if (!Heap::IsValueOfType(value, type.array->element)) {
          return std::unexpected(ExecutionStatus::InvalidCast);
        }
      }
    } else if (const auto source = bound.source_indices.at(index); source != kUnassigned) {
      if (!Heap::IsValueOfType(*Heap::GetElement(arguments, source), type)) {
        return std::unexpected(ExecutionStatus::InvalidCast);
      }
    }
  }
  return {};
}

std::expected<Value, ExecutionStatus> ReadDynamicArgument(Heap& heap,
                                                          const CallSignature& signature,
                                                          std::span<const TypeConstraint> types,
                                                          const HeapObject& arguments,
                                                          const BoundArguments& bound,
                                                          std::size_t index) {
  const auto& parameter = signature.parameters.at(bound.skipped + index);
  const auto& type = types.subspan(parameter.type).front();
  if (parameter.rest) {
    return PackRest(heap, arguments, bound, *type.array);
  }
  const auto source = bound.source_indices.at(index);
  return source == kUnassigned ? Value::Null()
                               : UnboxDynamicValue(*Heap::GetElement(arguments, source), type);
}

}  // namespace vela::runtime
