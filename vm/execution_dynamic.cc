#include "base/builtin.h"
#include "base/member.h"
#include "vm/execution.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

#include "base/callable.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"
#include "vm/program.h"

namespace vela::vm::internal {

StepResult Execution::ExecuteTypeBinding(ActiveFrame& active, Instruction instruction) {
  const auto builtin = static_cast<Builtin>(DecodeB(instruction));
  const auto values = active.frame.registers();
  const auto arguments = values.subspan(DecodeC(instruction), 2);
  const auto index = arguments.subspan(1).front();
  const auto types = active.function->layouts->types();
  if (!index.IsInteger() || index.AsInteger() < 0 ||
      static_cast<std::size_t>(index.AsInteger()) >= types.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& type = types.subspan(static_cast<std::size_t>(index.AsInteger())).front();
  const auto value = arguments.front();
  if (builtin == Builtin::CheckFuturePayload) {
    if (type.kind != runtime::ValueKind::Object ||
        type.object_kind != runtime::ObjectKind::Future || type.signature == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto payload = type.signature->result;
    if (!runtime::Heap::IsValueOfType(value.Read(), type) &&
        !(payload ? runtime::Heap::IsValueOfType(value.Read(), *payload) : value.IsNull())) {
      return std::unexpected(ExecutionStatus::InvalidCast);
    }
    values.subspan(DecodeA(instruction)).front() = value;
    return {};
  }
  if (value.IsNull() && type.nullable) {
    values.subspan(DecodeA(instruction)).front().SetNull();
    return {};
  }
  if (!value.IsObject()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* layout = isolate_.CreateArrayLayout({.element = type});
  if (auto bound = runtime::Heap::BindRuntimeType(*value.AsObject(), *layout); !bound) {
    return bound;
  }
  values.subspan(DecodeA(instruction)).front() = value;
  return {};
}

std::expected<runtime::Value, ExecutionStatus> Execution::BoxDynamicValue(
    runtime::Value value,
    const runtime::TypeConstraint& type) {
  const auto* instance = type.instance;
  if (value.IsNull() || instance == nullptr || !instance->IsEnum()) {
    return value;
  }
  auto* object = isolate_.heap().AllocateInstance(*instance, std::span(&value, 1));
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return runtime::Value::Object(object);
}

std::expected<Execution::ResolvedCallable, ExecutionStatus> Execution::ResolveCallable(
    runtime::Value value) const {
  const auto* layout = value.IsObject() ? runtime::Heap::ClassOf(*value.AsObject()) : nullptr;
  if (layout == nullptr || !layout->callable() || !program_.HasCallable(*layout)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto target = *layout->callable();
  ResolvedCallable result{.function = target.function};
  if (target.mode != CallableMode::InterfaceMethod && result.function >= functions_.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  switch (target.mode) {
    case CallableMode::Plain:
      return result;
    case CallableMode::Closure:
      result.implicit = value;
      return result;
    case CallableMode::BoundMethod:
    case CallableMode::InterfaceMethod: {
      const auto* receiver = runtime::Heap::GetField(*value.AsObject(), 0);
      if (receiver == nullptr) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      result.implicit = *receiver;
      break;
    }
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
  if (target.mode == CallableMode::InterfaceMethod) {
    const auto method = InterfaceTarget(*result.implicit, target.function);
    if (!method) {
      return std::unexpected(method.error());
    }
    result.function = *method;
  }
  return result;
}

std::expected<Execution::BoundArguments, ExecutionStatus> Execution::BindDynamicArguments(
    const CallSignature& signature,
    const ResolvedCallable& callable,
    const runtime::HeapObject& arguments,
    const runtime::HeapObject& names,
    DynamicCallStyle style) {
  const auto count = runtime::Heap::ArrayLength(arguments);
  const auto names_count = runtime::Heap::ArrayLength(names);
  if (!count || !names_count || *count != *names_count) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto hidden = callable.implicit ? 1U : 0U;
  if (hidden < signature.hidden_parameters) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  ArgumentBinding binding;
  auto& bound = binding.bound;
  bound.source_indices.fill(std::numeric_limits<std::uint32_t>::max());
  bound.rest_start = *count;
  bound.skipped = hidden - signature.hidden_parameters;
  if (signature.parameters.size() < bound.skipped) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  bound.count = static_cast<std::uint32_t>(signature.parameters.size()) - bound.skipped;
  for (std::uint32_t index = 0; index < *count; ++index) {
    const auto name = *runtime::Heap::GetElement(names, index);
    if (!name.IsString()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (auto result =
            BindDynamicArgument(signature, name.AsString()->value(), index, style, binding);
        !result) {
      return std::unexpected(result.error());
    }
  }
  for (std::uint32_t index = 0; index < bound.count; ++index) {
    const auto& parameter = signature.parameters.at(bound.skipped + index);
    if (!binding.assigned.at(index) && !parameter.optional && !parameter.rest) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  return bound;
}

StepResult Execution::BindDynamicArgument(const CallSignature& signature,
                                          std::string_view name,
                                          std::uint32_t index,
                                          DynamicCallStyle style,
                                          ArgumentBinding& binding) {
  auto& bound = binding.bound;
  std::optional<std::uint32_t> parameter;
  if (name.empty() && !binding.named) {
    if (!signature.parameters.empty() && signature.parameters.back().rest &&
        binding.positional + bound.skipped >= signature.parameters.size() - 1) {
      if (bound.rest_count == 0) {
        bound.rest_start = index;
      }
      ++bound.rest_count;
      return {};
    }
    parameter = binding.positional++ + bound.skipped;
  } else if (!name.empty() && style == DynamicCallStyle::Method) {
    binding.named = true;
    parameter = signature.ParameterIndex(name);
  }
  if (!parameter || *parameter < bound.skipped || *parameter >= signature.parameters.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto destination = *parameter - bound.skipped;
  if (binding.assigned.at(destination) || signature.parameters.at(*parameter).rest) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  binding.assigned.at(destination) = true;
  bound.source_indices.at(destination) = index;
  return {};
}

StepResult Execution::EnterDynamicCall(ActiveFrame& active,
                                       const ResolvedCallable& callable,
                                       const runtime::HeapObject& arguments,
                                       const runtime::HeapObject& names,
                                       std::uint8_t destination,
                                       DynamicCallStyle style) {
  const auto& function = functions_.subspan(callable.function).front();
  const auto& signature = function.signature;
  if (!signature) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& call_signature = *signature;
  const auto types = function.View().types;
  const auto bound = BindDynamicArguments(call_signature, callable, arguments, names, style);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  if (auto checked = CheckDynamicArguments(call_signature, types, arguments, *bound); !checked) {
    return checked;
  }
  const auto entered = EnterFrame({.function = callable.function, .destination = destination});
  if (!entered) {
    return std::unexpected(entered.error());
  }
  auto parameters = *entered;
  if (callable.implicit) {
    parameters.front() = *callable.implicit;
    parameters = parameters.subspan(1);
  }
  if (auto copied = CopyDynamicArguments(call_signature, types, arguments, *bound, parameters);
      !copied) {
    return copied;
  }
  active.dynamic_return = true;
  return {};
}

StepResult Execution::CheckDynamicArguments(const CallSignature& signature,
                                            std::span<const runtime::TypeConstraint> types,
                                            const runtime::HeapObject& arguments,
                                            const BoundArguments& bound) {
  for (std::uint32_t index = 0; index < bound.count; ++index) {
    const auto& parameter = signature.parameters.at(bound.skipped + index);
    const auto& type = types.subspan(parameter.type).front();
    if (parameter.rest) {
      for (std::uint32_t item = 0; item < bound.rest_count; ++item) {
        const auto value = *runtime::Heap::GetElement(arguments, bound.rest_start + item);
        if (!runtime::Heap::IsValueOfType(value, type.array->element)) {
          return std::unexpected(ExecutionStatus::InvalidCast);
        }
      }
      continue;
    }
    const auto source = bound.source_indices.at(index);
    if (source == std::numeric_limits<std::uint32_t>::max()) {
      continue;
    }
    const auto value = *runtime::Heap::GetElement(arguments, source);
    if (!runtime::Heap::IsValueOfType(value, type)) {
      return std::unexpected(ExecutionStatus::InvalidCast);
    }
  }
  return {};
}

StepResult Execution::CopyDynamicArguments(const CallSignature& signature,
                                           std::span<const runtime::TypeConstraint> types,
                                           const runtime::HeapObject& arguments,
                                           const BoundArguments& bound,
                                           runtime::RegisterView parameters) {
  for (std::uint32_t index = 0; index < bound.count; ++index) {
    const auto& parameter = signature.parameters.at(bound.skipped + index);
    const auto& type = types.subspan(parameter.type).front();
    auto destination_slot = parameters.subspan(index).front();
    if (parameter.rest) {
      const auto array = PackDynamicRest(arguments, bound, *type.array);
      if (!array) {
        return std::unexpected(array.error());
      }
      destination_slot.SetObject(*array);
      continue;
    }
    const auto source = bound.source_indices.at(index);
    if (source == std::numeric_limits<std::uint32_t>::max()) {
      destination_slot.SetNull();
      continue;
    }
    auto value = *runtime::Heap::GetElement(arguments, source);
    if (!value.IsNull() && type.instance != nullptr && type.instance->IsEnum()) {
      value = *runtime::Heap::GetField(*value.AsObject(), 0);
    }
    destination_slot = value;
  }
  return {};
}

std::expected<runtime::HeapObject*, ExecutionStatus> Execution::PackDynamicRest(
    const runtime::HeapObject& arguments,
    const BoundArguments& bound,
    const runtime::ArrayLayout& type) {
  auto* array = isolate_.heap().AllocateArray(type, bound.rest_count);
  if (array == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  for (std::uint32_t item = 0; item < bound.rest_count; ++item) {
    auto value = *runtime::Heap::GetElement(arguments, bound.rest_start + item);
    const auto* instance = type.element.instance;
    if (!value.IsNull() && instance != nullptr && instance->IsEnum()) {
      value = *runtime::Heap::GetField(*value.AsObject(), 0);
    }
    std::ignore = isolate_.heap().SetElement(*array, item, value);
  }
  return array;
}

StepResult Execution::ExecuteDynamicCall(ActiveFrame& active, Instruction instruction) {
  const auto input = active.frame.registers().subspan(DecodeC(instruction), 3);
  const auto callable = ResolveCallable(input.front().Read());
  const auto arguments = input.subspan(1).front().Read();
  const auto names = input.subspan(2).front().Read();
  if (!callable) {
    return std::unexpected(callable.error());
  }
  if (!arguments.IsObject() || !names.IsObject()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return EnterDynamicCall(active, *callable, *arguments.AsObject(), *names.AsObject(),
                          DecodeA(instruction), DynamicCallStyle::Function);
}

std::expected<runtime::Value, ExecutionStatus> Execution::CreateDynamicCallPlan(
    runtime::Value value,
    DynamicCallStyle style,
    std::optional<std::uint32_t> target) {
  if (style == DynamicCallStyle::Function) {
    if (const auto callable = ResolveCallable(value); !callable) {
      return std::unexpected(callable.error());
    }
  }
  auto& heap = isolate_.heap();
  auto* plan = heap.AllocateValueSlots(3);
  if (plan == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(*plan, 0, value);
  heap.SetValueSlot(*plan, 1, runtime::Value::Integer(static_cast<std::int64_t>(style)));
  if (target) {
    heap.SetValueSlot(*plan, 2, runtime::Value::Integer(*target));
  }
  return runtime::Value::Object(plan);
}

StepResult Execution::ExecuteDynamicMember(ActiveFrame& active, Instruction instruction) {
  const auto input = active.frame.registers().subspan(DecodeC(instruction), 2);
  const auto receiver = input.front().Read();
  const auto name = input.subspan(1).front().Read();
  if (!name.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* member = active.function->layouts->FindMember(receiver, name.AsString()->value());
  if (member == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (static_cast<Builtin>(DecodeB(instruction)) == Builtin::DynamicMemberSet) {
    return WriteDynamicMember(active, instruction, *member);
  }
  return ReadDynamicMember(active, instruction, *member);
}

StepResult Execution::ReadDynamicMember(ActiveFrame& active,
                                        Instruction instruction,
                                        const MemberDefinition& member) {
  const auto values = active.frame.registers();
  const auto receiver = values.subspan(DecodeC(instruction)).front().Read();
  const auto resolving = static_cast<Builtin>(DecodeB(instruction)) == Builtin::DynamicResolveCall;
  auto destination = values.subspan(DecodeA(instruction)).front();
  const auto& layouts = *active.function->layouts;
  switch (member.kind) {
    case MemberKind::Field: {
      const auto type = member.type;
      assert(type.has_value());
      const auto value = *runtime::Heap::GetField(*receiver.AsObject(), member.target);
      const auto boxed = BoxDynamicValue(value, layouts.types().subspan(*type).front());
      if (!boxed) {
        return std::unexpected(boxed.error());
      }
      destination = *boxed;
      break;
    }
    case MemberKind::Method: {
      const auto& wrapper = *layouts.classes().subspan(member.target).front();
      const auto target = wrapper.callable();
      assert(target.has_value());
      if (resolving) {
        const auto plan =
            CreateDynamicCallPlan(receiver, DynamicCallStyle::Method, (*target).function);
        if (!plan) {
          return std::unexpected(plan.error());
        }
        destination = *plan;
        return {};
      }
      auto* callable = isolate_.heap().AllocateInstance(wrapper, std::span(&receiver, 1));
      if (callable == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      destination.SetObject(callable);
      return {};
    }
    case MemberKind::Property: {
      if (member.target == kInvalidFunctionTarget) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const auto parameters =
          EnterFrame({.function = member.target, .destination = DecodeA(instruction)});
      if (!parameters) {
        return std::unexpected(parameters.error());
      }
      parameters->front() = receiver;
      active.dynamic_return = true;
      active.dynamic_call_plan = resolving;
      return {};
    }
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
  if (resolving) {
    const auto plan = CreateDynamicCallPlan(destination.Read(), DynamicCallStyle::Function);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    destination = *plan;
  }
  return {};
}

StepResult Execution::WriteDynamicMember(ActiveFrame& active,
                                         Instruction instruction,
                                         const MemberDefinition& member) {
  const auto values = active.frame.registers();
  const auto input_register = DecodeC(instruction);
  const auto input = values.subspan(input_register, 3);
  const auto receiver = input.front().Read();
  auto value = input.subspan(2).front().Read();
  const runtime::TypeConstraint* type = nullptr;
  if (member.kind == MemberKind::Field) {
    const auto index = member.type;
    assert(index.has_value());
    type = &active.function->layouts->types().subspan(*index).front();
  } else if (member.kind == MemberKind::Property && member.setter) {
    const auto& setter = functions_.subspan(*member.setter).front();
    const auto& signature = setter.signature;
    assert(signature.has_value());
    const auto parameter = (*signature).parameters.front().type;
    type = &setter.layouts->types().subspan(parameter).front();
  } else {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!runtime::Heap::IsValueOfType(value, *type)) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  }
  if (!value.IsNull() && type->instance != nullptr && type->instance->IsEnum()) {
    value = *runtime::Heap::GetField(*value.AsObject(), 0);
  }
  if (member.kind == MemberKind::Field) {
    static_cast<void>(isolate_.heap().SetField(*receiver.AsObject(), member.target, value));
    values.subspan(DecodeA(instruction)).front() = input.subspan(2).front();
    return {};
  }
  assert(member.setter.has_value());
  const auto parameters =
      EnterFrame({.function = *member.setter, .destination = DecodeA(instruction)});
  if (!parameters) {
    return std::unexpected(parameters.error());
  }
  parameters->front() = receiver;
  parameters->subspan(1).front() = value;
  active.dynamic_assignment = static_cast<std::uint8_t>(input_register + 2);
  return {};
}

StepResult Execution::ExecuteDynamicMethodCall(ActiveFrame& active, Instruction instruction) {
  const auto input = active.frame.registers().subspan(DecodeC(instruction), 3);
  const auto plan_value = input.front().Read();
  const auto arguments = input.subspan(1).front().Read();
  const auto names = input.subspan(2).front().Read();
  if (!plan_value.IsObject() ||
      runtime::Heap::KindOf(*plan_value.AsObject()) != runtime::ObjectKind::ValueSlots ||
      !arguments.IsObject() || !names.IsObject()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto plan = runtime::Heap::ValueSlots(*plan_value.AsObject());
  if (plan.size() != 3 || !plan.subspan(1).front().IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto style = static_cast<DynamicCallStyle>(plan.subspan(1).front().AsInteger());
  std::expected<ResolvedCallable, ExecutionStatus> callable;
  if (style == DynamicCallStyle::Method) {
    const auto target = plan.subspan(2).front().AsInteger();
    callable =
        ResolvedCallable{.function = static_cast<std::uint32_t>(target), .implicit = plan.front()};
  } else {
    callable = ResolveCallable(plan.front());
  }
  if (!callable) {
    return std::unexpected(callable.error());
  }
  return EnterDynamicCall(active, *callable, *arguments.AsObject(), *names.AsObject(),
                          DecodeA(instruction), style);
}

}  // namespace vela::vm::internal
