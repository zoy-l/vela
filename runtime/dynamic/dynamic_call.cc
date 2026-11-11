#include "runtime/dynamic/dynamic_call.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

#include <absl/functional/function_ref.h>

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
  const RootRange root(heap, &plan.value, 1, std::nullopt);
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

DynamicCallPlan ReadDynamicCallPlan(Value value) {
  const auto storage = Heap::ValueSlots(*value.AsObject());
  const auto method = storage.subspan(1).front();
  return DynamicCallPlan{
      .value = storage.front(),
      .method = method.IsNull() ? std::nullopt
                                : std::optional(static_cast<std::uint32_t>(method.AsInteger())),
  };
}

namespace {

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
    if (source != BoundArguments::kUnassigned || parameters.at(*parameter).rest) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    source = index;
    return {};
  }
};

std::expected<Value, ExecutionStatus> PackRest(Heap& heap,
                                               absl::FunctionRef<Value(std::uint32_t)> argument,
                                               const BoundArguments& bound,
                                               const ArrayLayout& type) {
  for (std::uint32_t item = 0; item < bound.rest_count; ++item) {
    if (!Heap::IsValueOfType(argument(bound.rest_start + item), type.element)) {
      return std::unexpected(ExecutionStatus::InvalidCast);
    }
  }
  auto* array = heap.AllocateArray(type, bound.rest_count);
  if (array == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  for (std::uint32_t item = 0; item < bound.rest_count; ++item) {
    const auto value = argument(bound.rest_start + item);
    std::ignore = heap.SetElement(*array, item, UnboxDynamicValue(value, type.element));
  }
  return Value::Object(array);
}

std::expected<void, ExecutionStatus> CheckRequiredArguments(const CallSignature& signature,
                                                            const BoundArguments& bound) {
  for (std::size_t index = 0; index < bound.source_indices.size(); ++index) {
    const auto& parameter = signature.parameters[bound.skipped + index];
    if (bound.source_indices[index] == BoundArguments::kUnassigned && !parameter.optional &&
        !parameter.rest) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  return {};
}

template <class NameAt>
std::expected<BoundArguments, ExecutionStatus> BindArguments(const CallSignature& signature,
                                                             std::uint32_t count,
                                                             std::uint32_t skipped,
                                                             DynamicCallStyle style,
                                                             const NameAt& name_at) {
  const auto& parameters = signature.parameters;
  if (parameters.size() < skipped) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  BoundArguments bound{
      .source_indices =
          BoundArguments::SourceIndices(parameters.size() - skipped, BoundArguments::kUnassigned),
      .skipped = skipped,
      .rest_start = count,
  };
  ArgumentBinding binding{.signature = &signature, .bound = &bound, .style = style};
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto name = name_at(index);
    if (!name) {
      return std::unexpected(name.error());
    }
    if (const auto result = binding.Bind(*name, index); !result) {
      return std::unexpected(result.error());
    }
  }
  if (const auto checked = CheckRequiredArguments(signature, bound); !checked) {
    return std::unexpected(checked.error());
  }
  return bound;
}

std::expected<Value, ExecutionStatus> ReadArgument(Heap& heap,
                                                   const CallSignature& signature,
                                                   std::span<const TypeConstraint> types,
                                                   absl::FunctionRef<Value(std::uint32_t)> argument,
                                                   const BoundArguments& bound,
                                                   std::size_t index) {
  const auto& parameter = signature.parameters[bound.skipped + index];
  const auto& type = types[parameter.type];
  if (parameter.rest) {
    return PackRest(heap, argument, bound, *type.array);
  }
  const auto source = bound.source_indices[index];
  if (source == BoundArguments::kUnassigned) {
    return Value::Null();
  }
  const auto value = argument(source);
  return Heap::IsValueOfType(value, type)
             ? std::expected<Value, ExecutionStatus>(UnboxDynamicValue(value, type))
             : std::unexpected(ExecutionStatus::InvalidCast);
}

}  // namespace

std::expected<BoundArguments, ExecutionStatus> BindDynamicArguments(
    const CallSignature& signature,
    const ResolvedCallable& callable,
    const HeapObject& arguments,
    const HeapObject* names,
    DynamicCallStyle style) {
  const auto count = Heap::ArrayLength(arguments);
  const auto hidden = callable.implicit ? 1U : 0U;
  if (!count || hidden < signature.hidden_parameters) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (names != nullptr) {
    const auto names_count = Heap::ArrayLength(*names);
    if (!names_count || *count != *names_count) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  return BindArguments(
      signature, *count, hidden - signature.hidden_parameters, style,
      [&](std::uint32_t index) -> std::expected<std::string_view, ExecutionStatus> {
        if (names == nullptr) {
          return std::string_view{};
        }
        const auto value = *Heap::GetElement(*names, index);
        return value.IsString()
                   ? std::expected<std::string_view, ExecutionStatus>(value.AsString()->value())
                   : std::unexpected(ExecutionStatus::TypeError);
      });
}

std::expected<BoundArguments, ExecutionStatus> BindDynamicArguments(
    const CallSignature& signature,
    std::span<const std::string_view> names,
    DynamicCallStyle style) {
  if (names.size() > BoundArguments::kUnassigned) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return BindArguments(
      signature, static_cast<std::uint32_t>(names.size()), 0, style,
      [&](std::uint32_t index) -> std::expected<std::string_view, ExecutionStatus> {
        return names[index];
      });
}

std::expected<Value, ExecutionStatus> ReadDynamicArgument(Heap& heap,
                                                          const CallSignature& signature,
                                                          std::span<const TypeConstraint> types,
                                                          const HeapObject& arguments,
                                                          const BoundArguments& bound,
                                                          std::size_t index) {
  return ReadArgument(
      heap, signature, types, [&](auto source) { return *Heap::GetElement(arguments, source); },
      bound, index);
}

std::expected<Value, ExecutionStatus> ReadDynamicArgument(Heap& heap,
                                                          const CallSignature& signature,
                                                          std::span<const TypeConstraint> types,
                                                          std::span<const Value> arguments,
                                                          const BoundArguments& bound,
                                                          std::size_t index) {
  return ReadArgument(
      heap, signature, types, [&](auto source) { return arguments[source]; }, bound, index);
}

}  // namespace vela::runtime
