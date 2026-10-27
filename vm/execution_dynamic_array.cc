#include "vm/execution.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <tuple>

#include "base/array_algorithm.h"
#include "base/builtin.h"
#include "base/callable.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"

namespace vela::vm::internal {
namespace {

constexpr std::size_t kReceiver = 0;
constexpr std::size_t kCallback = 1;
constexpr std::size_t kArguments = 2;
constexpr std::size_t kNames = 3;
constexpr std::size_t kOutput = 4;
constexpr std::size_t kAlgorithm = 5;
constexpr std::size_t kThunk = 6;
constexpr std::size_t kSlotCount = 7;

std::expected<std::int64_t, ExecutionStatus> ComparatorSign(runtime::Value value) {
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

std::expected<std::span<runtime::Value>, ExecutionStatus> ArrayPlan(runtime::Value value) {
  if (!value.IsObject() ||
      runtime::Heap::KindOf(*value.AsObject()) != runtime::ObjectKind::ValueSlots) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto slots = runtime::Heap::ValueSlots(*value.AsObject());
  if (slots.size() != kSlotCount) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return slots;
}

StepResult CheckCallback(const runtime::TypeSignature& callback,
                         ArrayAlgorithm algorithm,
                         runtime::Value receiver,
                         const runtime::TypeConstraint& accumulator) {
  const auto* layout = runtime::Heap::ArrayTypeOf(*receiver.AsObject());
  const auto element = layout->value_type();
  const runtime::TypeConstraint integer{.kind = runtime::ValueKind::Integer};
  const auto array = runtime::Heap::TypeOf(receiver);
  auto arguments = std::array{element, integer, array, accumulator};
  std::size_t count = 3;
  if (algorithm == ArrayAlgorithm::Sort) {
    arguments.at(1) = element;
    count = 2;
  } else if (algorithm == ArrayAlgorithm::Reduce) {
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
        (result.kind == runtime::ValueKind::Integer || result.kind == runtime::ValueKind::Double)) {
      return {};
    }
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto expected = algorithm == ArrayAlgorithm::Reduce
                            ? accumulator
                            : runtime::TypeConstraint{.kind = runtime::ValueKind::Boolean};
  return result.dynamic || result.IsSubtypeOf(expected)
             ? StepResult{}
             : std::unexpected(ExecutionStatus::TypeError);
}

std::expected<runtime::HeapObject*, ExecutionStatus> AllocateOutput(runtime::Isolate& isolate,
                                                                    runtime::TypeConstraint type,
                                                                    std::uint32_t length) {
  if (type.instance != nullptr && type.instance->IsEnum()) {
    type.kind = runtime::ValueKind::Integer;
  }
  const auto* layout = isolate.CreateArrayLayout({.element = type});
  auto* output = isolate.heap().AllocateArray(*layout, length);
  return output == nullptr ? std::expected<runtime::HeapObject*, ExecutionStatus>{std::unexpected(
                                 ExecutionStatus::OutOfMemory)}
                           : output;
}

std::expected<runtime::TypeConstraint, ExecutionStatus> AccumulatorType(
    runtime::Value initial,
    ArrayAlgorithm algorithm,
    const runtime::TypeSignature& callback) {
  if (algorithm != ArrayAlgorithm::Reduce || !initial.IsNull()) {
    return runtime::Heap::TypeOf(initial);
  }
  const auto result = callback.result;
  if (!result) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto type = callback.parameters.empty() ? *result : callback.parameters.front().type;
  if (!runtime::Heap::IsValueOfType(initial, type)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return type;
}

StepResult InitializeArguments(runtime::Isolate& isolate,
                               runtime::HeapObject& plan,
                               std::uint32_t count) {
  auto& heap = isolate.heap();
  const auto* values = isolate.CreateArrayLayout({.element = {.dynamic = true}});
  auto* arguments = heap.AllocateArray(*values, count);
  if (arguments == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(plan, kArguments, runtime::Value::Object(arguments));
  const auto* strings =
      isolate.CreateArrayLayout({.element = {.kind = runtime::ValueKind::String}});
  auto* names = heap.AllocateArray(*strings, count);
  if (names == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(plan, kNames, runtime::Value::Object(names));
  if (count != 0) {
    auto* empty = heap.AllocateString("");
    if (empty == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    for (std::uint32_t index = 0; index < count; ++index) {
      std::ignore = heap.SetElement(*names, index, runtime::Value::String(empty));
    }
  }
  return {};
}

StepResult InitializeOutput(runtime::Isolate& isolate,
                            runtime::HeapObject& plan,
                            const runtime::TypeSignature* signature,
                            std::uint32_t length,
                            const runtime::TypeConstraint& accumulator) {
  const auto slots = runtime::Heap::ValueSlots(plan);
  const auto algorithm = static_cast<ArrayAlgorithm>(slots.subspan(kAlgorithm).front().AsInteger());
  auto& heap = isolate.heap();
  if (algorithm == ArrayAlgorithm::Map) {
    const auto type = signature->result;
    assert(type.has_value());
    const auto output = AllocateOutput(isolate, *type, length);
    if (!output) {
      return std::unexpected(output.error());
    }
    heap.SetValueSlot(plan, kOutput, runtime::Value::Object(*output));
  } else if (algorithm == ArrayAlgorithm::Reduce) {
    const auto output = AllocateOutput(isolate, accumulator, 0);
    if (!output) {
      return std::unexpected(output.error());
    }
    heap.SetValueSlot(plan, kOutput, runtime::Value::Object(*output));
  }
  return {};
}

}  // namespace

StepResult Execution::PrepareDynamicArray(ActiveFrame& active, Instruction instruction) {
  const auto input = active.frame.registers().subspan(DecodeC(instruction), kDynamicOperationArity);
  const auto receiver = input.front().Read();
  const auto callback = input.subspan(1).front().Read();
  const auto initial = input.subspan(2).front().Read();
  const auto selector = input.subspan(3).front().Read();
  const auto wrapper = input.subspan(4).front().Read();
  const auto classes = active.function->layouts->classes();
  if (!receiver.IsObject() || !selector.IsInteger() || selector.AsInteger() < 0 ||
      selector.AsInteger() > static_cast<std::int64_t>(ArrayAlgorithm::Map) ||
      !wrapper.IsInteger() || wrapper.AsInteger() < 0 ||
      static_cast<std::size_t>(wrapper.AsInteger()) >= classes.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto length = runtime::Heap::ArrayLength(*receiver.AsObject());
  if (!length) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* closure = classes.subspan(static_cast<std::size_t>(wrapper.AsInteger())).front();
  if (closure == nullptr || !closure->callable() ||
      closure->callable()->mode != CallableMode::Closure || closure->field_count() != 1) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto algorithm = static_cast<ArrayAlgorithm>(selector.AsInteger());
  auto accumulator = runtime::Heap::TypeOf(initial);
  const runtime::TypeSignature* signature = nullptr;
  if (const auto callable = ResolveCallable(callback); !callable) {
    return std::unexpected(callable.error());
  }
  signature = runtime::Heap::ClassOf(*callback.AsObject())->callable_type();
  if (signature == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto type = AccumulatorType(initial, algorithm, *signature);
  if (!type) {
    return std::unexpected(type.error());
  }
  accumulator = *type;
  if (auto checked = CheckCallback(*signature, algorithm, receiver, accumulator); !checked) {
    return checked;
  }
  auto& heap = isolate_.heap();
  auto* plan = heap.AllocateValueSlots(kSlotCount);
  if (plan == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  active.frame.registers().subspan(DecodeA(instruction)).front().SetObject(plan);
  heap.SetValueSlot(*plan, kReceiver, receiver);
  heap.SetValueSlot(*plan, kCallback, callback);
  heap.SetValueSlot(*plan, kAlgorithm, selector);
  const auto count = static_cast<std::uint32_t>(signature->parameters.size());
  if (auto initialized = InitializeArguments(isolate_, *plan, count); !initialized) {
    return initialized;
  }
  if (auto initialized = InitializeOutput(isolate_, *plan, signature, *length, accumulator);
      !initialized) {
    return initialized;
  }
  const auto context = runtime::Value::Object(plan);
  auto* thunk = heap.AllocateInstance(*closure, std::span(&context, 1));
  if (thunk == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(*plan, kThunk, runtime::Value::Object(thunk));
  return {};
}

StepResult Execution::InvokeDynamicArray(ActiveFrame& active, Instruction instruction) {
  const auto input = active.frame.registers().subspan(DecodeC(instruction), kDynamicOperationArity);
  const auto plan = ArrayPlan(input.front().Read());
  if (!plan) {
    return std::unexpected(plan.error());
  }
  const auto slots = *plan;
  const auto algorithm = static_cast<ArrayAlgorithm>(slots.subspan(kAlgorithm).front().AsInteger());
  const auto callback = slots.subspan(kCallback).front();
  const auto* source = runtime::Heap::ArrayTypeOf(*slots.subspan(kReceiver).front().AsObject());
  auto& arguments = *slots.subspan(kArguments).front().AsObject();
  const auto count = *runtime::Heap::ArrayLength(arguments);
  for (std::uint32_t index = 0; index < count; ++index) {
    auto value = input.subspan(index + 1).front().Read();
    const bool element = algorithm == ArrayAlgorithm::Sort ||
                         index == (algorithm == ArrayAlgorithm::Reduce ? 1U : 0U);
    if (element) {
      const auto boxed = BoxDynamicValue(value, source->element);
      if (!boxed) {
        return std::unexpected(boxed.error());
      }
      value = *boxed;
    }
    std::ignore = isolate_.heap().SetElement(arguments, index, value);
  }
  const auto callable = ResolveCallable(callback);
  if (!callable) {
    return std::unexpected(callable.error());
  }
  return EnterDynamicCall(active, *callable, arguments, *slots.subspan(kNames).front().AsObject(),
                          DecodeA(instruction), DynamicCallStyle::Function);
}

StepResult Execution::ExecuteDynamicArray(ActiveFrame& active, Instruction instruction) {
  const auto builtin = static_cast<Builtin>(DecodeB(instruction));
  if (builtin == Builtin::DynamicArrayPrepare) {
    return PrepareDynamicArray(active, instruction);
  }
  if (builtin == Builtin::DynamicArrayInvoke) {
    return InvokeDynamicArray(active, instruction);
  }
  const auto input = active.frame.registers().subspan(DecodeC(instruction), BuiltinArity(builtin));
  const auto plan = ArrayPlan(input.front().Read());
  if (!plan) {
    return std::unexpected(plan.error());
  }
  const auto slots = *plan;
  auto destination = active.frame.registers().subspan(DecodeA(instruction)).front();
  if (builtin == Builtin::DynamicArrayCallback || builtin == Builtin::DynamicArrayOutput) {
    destination =
        slots.subspan(builtin == Builtin::DynamicArrayCallback ? kThunk : kOutput).front();
    return {};
  }
  const auto value = input.subspan(1).front().Read();
  const auto algorithm = static_cast<ArrayAlgorithm>(slots.subspan(kAlgorithm).front().AsInteger());
  if (algorithm == ArrayAlgorithm::Sort) {
    const auto result = ComparatorSign(value);
    if (!result) {
      return std::unexpected(result.error());
    }
    destination.SetInteger(*result);
  } else if (algorithm == ArrayAlgorithm::Reduce) {
    const auto type =
        runtime::Heap::ArrayTypeOf(*slots.subspan(kOutput).front().AsObject())->value_type();
    if (!runtime::Heap::IsValueOfType(value, type)) {
      return std::unexpected(ExecutionStatus::InvalidCast);
    }
    destination = value;
  } else if (algorithm != ArrayAlgorithm::Map && algorithm != ArrayAlgorithm::ForEach &&
             !value.IsBoolean()) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  } else {
    destination = value;
  }
  return {};
}

}  // namespace vela::vm::internal
