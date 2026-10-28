#include "runtime/dynamic_array.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <tuple>
#include <utility>

#include "base/array_algorithm.h"
#include "base/builtin.h"
#include "base/callable.h"
#include "base/execution_status.h"
#include "runtime/callable.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "runtime/value_operations.h"

namespace vela::runtime {
namespace {

using OperationResult = std::expected<void, ExecutionStatus>;

constexpr std::size_t kReceiver = 0;
constexpr std::size_t kCallback = 1;
constexpr std::size_t kArguments = 2;
constexpr std::size_t kNames = 3;
constexpr std::size_t kOutput = 4;
constexpr std::size_t kAlgorithm = 5;
constexpr std::size_t kThunk = 6;
constexpr std::size_t kSlotCount = 7;

std::expected<std::int64_t, ExecutionStatus> ComparatorSign(Value value) {
  if (value.IsInteger()) {
    const auto number = value.AsInteger();
    return static_cast<std::int64_t>(number > 0) - static_cast<std::int64_t>(number < 0);
  }
  if (value.IsDouble()) {
    const auto number = value.AsDouble();
    return static_cast<std::int64_t>(number > 0) - static_cast<std::int64_t>(number < 0);
  }
  return std::unexpected(ExecutionStatus::TypeError);
}

std::expected<std::span<Value>, ExecutionStatus> ArrayPlan(Value value) {
  if (!value.IsObject() || Heap::KindOf(*value.AsObject()) != ObjectKind::ValueSlots) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto slots = Heap::ValueSlots(*value.AsObject());
  if (slots.size() != kSlotCount) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return slots;
}

OperationResult CheckCallback(const TypeSignature& callback,
                              ArrayAlgorithm algorithm,
                              Value receiver,
                              const TypeConstraint& accumulator) {
  const auto* layout = Heap::ArrayTypeOf(*receiver.AsObject());
  const auto element = layout->value_type();
  const TypeConstraint integer{.kind = ValueKind::Integer};
  const auto array = Heap::TypeOf(receiver);
  auto arguments = std::array{element, integer, array, accumulator};
  std::size_t count = 3;
  if (algorithm == ArrayAlgorithm::Sort) {
    arguments.at(1) = element;
    count = 2;
  }
  if (algorithm == ArrayAlgorithm::Reduce) {
    arguments = {accumulator, element, integer, array};
    count = arguments.size();
  }
  if (callback.parameters.size() > count) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  for (std::size_t index = 0; index < callback.parameters.size(); ++index) {
    const auto& parameter = callback.parameters.at(index);
    const auto& actual = arguments.at(index);
    if (parameter.optional || parameter.rest ||
        (!actual.dynamic && !actual.IsSubtypeOf(parameter.type))) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  if (algorithm == ArrayAlgorithm::ForEach) {
    return {};
  }
  if (!callback.result) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto result = *callback.result;
  if (algorithm == ArrayAlgorithm::Map) {
    return {};
  }
  if (algorithm == ArrayAlgorithm::Sort) {
    if (!result.nullable &&
        (result.kind == ValueKind::Integer || result.kind == ValueKind::Double)) {
      return {};
    }
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto expected = algorithm == ArrayAlgorithm::Reduce
                            ? accumulator
                            : TypeConstraint{.kind = ValueKind::Boolean};
  return result.dynamic || result.IsSubtypeOf(expected)
             ? OperationResult{}
             : std::unexpected(ExecutionStatus::TypeError);
}

std::expected<HeapObject*, ExecutionStatus> AllocateOutput(Isolate& isolate,
                                                           TypeConstraint type,
                                                           std::uint32_t length) {
  if (type.instance != nullptr && type.instance->IsEnum()) {
    type.kind = ValueKind::Integer;
  }
  const auto* layout = isolate.CreateArrayLayout({.element = type});
  auto* output = isolate.heap().AllocateArray(*layout, length);
  return output == nullptr ? std::expected<HeapObject*, ExecutionStatus>{std::unexpected(
                                 ExecutionStatus::OutOfMemory)}
                           : output;
}

std::expected<TypeConstraint, ExecutionStatus> AccumulatorType(Value initial,
                                                               ArrayAlgorithm algorithm,
                                                               const TypeSignature& callback) {
  if (algorithm != ArrayAlgorithm::Reduce || !initial.IsNull()) {
    return Heap::TypeOf(initial);
  }
  const auto result = callback.result;
  if (!result) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto type = callback.parameters.empty() ? *result : callback.parameters.front().type;
  if (!Heap::IsValueOfType(initial, type)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return type;
}

OperationResult InitializeArguments(Isolate& isolate, HeapObject& plan, std::uint32_t count) {
  auto& heap = isolate.heap();
  const auto* values = isolate.CreateArrayLayout({.element = {.dynamic = true}});
  auto* arguments = heap.AllocateArray(*values, count);
  if (arguments == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(plan, kArguments, Value::Object(arguments));
  const auto* strings = isolate.CreateArrayLayout({.element = {.kind = ValueKind::String}});
  auto* names = heap.AllocateArray(*strings, count);
  if (names == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(plan, kNames, Value::Object(names));
  if (count != 0) {
    auto* empty = heap.AllocateString("");
    if (empty == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    for (std::uint32_t index = 0; index < count; ++index) {
      std::ignore = heap.SetElement(*names, index, Value::String(empty));
    }
  }
  return {};
}

OperationResult InitializeOutput(Isolate& isolate,
                                 HeapObject& plan,
                                 const TypeSignature* signature,
                                 std::uint32_t length,
                                 const TypeConstraint& accumulator) {
  const auto slots = Heap::ValueSlots(plan);
  const auto algorithm = static_cast<ArrayAlgorithm>(slots.subspan(kAlgorithm).front().AsInteger());
  auto& heap = isolate.heap();
  if (algorithm == ArrayAlgorithm::Map) {
    const auto& type = signature->result;
    if (!type) {
      std::unreachable();
    }
    const auto output = AllocateOutput(isolate, *type, length);
    if (!output) {
      return std::unexpected(output.error());
    }
    heap.SetValueSlot(plan, kOutput, Value::Object(*output));
  } else if (algorithm == ArrayAlgorithm::Reduce) {
    const auto output = AllocateOutput(isolate, accumulator, 0);
    if (!output) {
      return std::unexpected(output.error());
    }
    heap.SetValueSlot(plan, kOutput, Value::Object(*output));
  }
  return {};
}

}  // namespace

std::expected<Value, ExecutionStatus> PrepareDynamicArray(Isolate& isolate,
                                                          std::span<const Value> input,
                                                          std::span<ClassLayout* const> classes,
                                                          const CodeIdentity& identity,
                                                          std::size_t function_count) {
  const auto receiver = input.front();
  const auto callback = input.subspan(1).front();
  const auto initial = input.subspan(2).front();
  const auto selector = input.subspan(3).front();
  const auto wrapper = input.subspan(4).front();
  if (!receiver.IsObject() || !selector.IsInteger() || selector.AsInteger() < 0 ||
      selector.AsInteger() > static_cast<std::int64_t>(ArrayAlgorithm::Map) ||
      !wrapper.IsInteger() || wrapper.AsInteger() < 0 ||
      static_cast<std::size_t>(wrapper.AsInteger()) >= classes.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto length = Heap::ArrayLength(*receiver.AsObject());
  if (!length) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* closure = classes.subspan(static_cast<std::size_t>(wrapper.AsInteger())).front();
  if (closure == nullptr || !closure->callable() ||
      closure->callable()->mode != CallableMode::Closure || closure->field_count() != 1) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto algorithm = static_cast<ArrayAlgorithm>(selector.AsInteger());
  auto accumulator = Heap::TypeOf(initial);
  const TypeSignature* signature = nullptr;
  if (const auto callable = ResolveCallable(callback, identity, function_count); !callable) {
    return std::unexpected(callable.error());
  }
  signature = Heap::ClassOf(*callback.AsObject())->callable_type();
  if (signature == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto type = AccumulatorType(initial, algorithm, *signature);
  if (!type) {
    return std::unexpected(type.error());
  }
  accumulator = *type;
  if (auto checked = CheckCallback(*signature, algorithm, receiver, accumulator); !checked) {
    return std::unexpected(checked.error());
  }
  auto& heap = isolate.heap();
  auto* plan = heap.AllocateValueSlots(kSlotCount);
  if (plan == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto owner = Value::Object(plan);
  const RootRange root(heap, &owner, 1);
  heap.SetValueSlot(*plan, kReceiver, receiver);
  heap.SetValueSlot(*plan, kCallback, callback);
  heap.SetValueSlot(*plan, kAlgorithm, selector);
  const auto count = static_cast<std::uint32_t>(signature->parameters.size());
  if (auto initialized = InitializeArguments(isolate, *plan, count); !initialized) {
    return std::unexpected(initialized.error());
  }
  if (auto initialized = InitializeOutput(isolate, *plan, signature, *length, accumulator);
      !initialized) {
    return std::unexpected(initialized.error());
  }
  const auto context = Value::Object(plan);
  auto* thunk = heap.AllocateInstance(*closure, std::span(&context, 1));
  if (thunk == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(*plan, kThunk, Value::Object(thunk));
  return owner;
}

std::expected<DynamicArrayInvocation, ExecutionStatus> PrepareDynamicArrayInvocation(
    Heap& heap,
    std::span<const Value> input) {
  const auto plan = ArrayPlan(input.front());
  if (!plan) {
    return std::unexpected(plan.error());
  }
  const auto slots = *plan;
  const auto algorithm = static_cast<ArrayAlgorithm>(slots.subspan(kAlgorithm).front().AsInteger());
  const auto callback = slots.subspan(kCallback).front();
  const auto* source = Heap::ArrayTypeOf(*slots.subspan(kReceiver).front().AsObject());
  auto& arguments = *slots.subspan(kArguments).front().AsObject();
  const auto count = *Heap::ArrayLength(arguments);
  for (std::uint32_t index = 0; index < count; ++index) {
    auto value = input.subspan(index + 1).front();
    const bool element = algorithm == ArrayAlgorithm::Sort ||
                         index == (algorithm == ArrayAlgorithm::Reduce ? 1U : 0U);
    if (element) {
      const auto boxed = BoxDynamicValue(heap, value, source->element);
      if (!boxed) {
        return std::unexpected(boxed.error());
      }
      value = *boxed;
    }
    std::ignore = heap.SetElement(arguments, index, value);
  }
  return DynamicArrayInvocation{
      .callback = callback,
      .arguments = &arguments,
      .names = slots.subspan(kNames).front().AsObject(),
  };
}

std::expected<Value, ExecutionStatus> ReadDynamicArrayOperation(Builtin builtin,
                                                                std::span<const Value> input) {
  const auto plan = ArrayPlan(input.front());
  if (!plan) {
    return std::unexpected(plan.error());
  }
  const auto slots = *plan;
  if (builtin == Builtin::DynamicArrayCallback || builtin == Builtin::DynamicArrayOutput) {
    return slots.subspan(builtin == Builtin::DynamicArrayCallback ? kThunk : kOutput).front();
  }
  const auto value = input.subspan(1).front();
  const auto algorithm = static_cast<ArrayAlgorithm>(slots.subspan(kAlgorithm).front().AsInteger());
  if (algorithm == ArrayAlgorithm::Sort) {
    const auto result = ComparatorSign(value);
    if (!result) {
      return std::unexpected(result.error());
    }
    return Value::Integer(*result);
  }
  if (algorithm == ArrayAlgorithm::Reduce) {
    const auto type = Heap::ArrayTypeOf(*slots.subspan(kOutput).front().AsObject())->value_type();
    if (!Heap::IsValueOfType(value, type)) {
      return std::unexpected(ExecutionStatus::InvalidCast);
    }
    return value;
  }
  if (algorithm != ArrayAlgorithm::Map && algorithm != ArrayAlgorithm::ForEach &&
      !value.IsBoolean()) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  }
  return value;
}

}  // namespace vela::runtime
