#include "base/builtin.h"
#include "base/member.h"
#include "vm/execution.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include "base/callable.h"
#include "runtime/dynamic/dynamic_call.h"
#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"
#include "runtime/value/callable.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"
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
  const auto offset = ReadLayoutIndex(index, types.size());
  if (!offset) {
    return std::unexpected(offset.error());
  }
  const auto& type = types.subspan(*offset).front();
  const auto value = arguments.front().Read();
  const auto result = builtin == Builtin::CheckFuturePayload
                          ? runtime::CheckFuturePayload(value, type)
                          : runtime::BindValueType(value, type);
  return StoreResult(values.subspan(DecodeA(instruction)).front(), result);
}

std::expected<std::size_t, ExecutionStatus> Execution::ReadLayoutIndex(runtime::Value value,
                                                                       std::size_t count) {
  if (!value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto index = value.AsInteger();
  if (index < 0 || std::cmp_greater_equal(index, count)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return static_cast<std::size_t>(index);
}

std::expected<runtime::Value, ExecutionStatus> Execution::BoxDynamicValue(
    runtime::Value value,
    const runtime::TypeConstraint& type) {
  return runtime::BoxDynamicValue(isolate_.heap(), value, type);
}

std::expected<Execution::ResolvedCallable, ExecutionStatus> Execution::ResolveCallable(
    runtime::Value value) const {
  return runtime::ResolveCallable(value, program_.code_identity(), functions_.size());
}

std::expected<Execution::CallableContract, ExecutionStatus> Execution::ResolveCallableContract(
    const ResolvedCallable& callable) const {
  const auto& function = functions_.subspan(callable.function).front();
  const auto& signature = function.signature;
  if (!signature) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto skipped = (callable.implicit ? 1U : 0U) - signature->hidden_parameters;
  return CallableContract{
      .signature = &*signature,
      .parameters = std::span(signature->parameters).subspan(skipped),
  };
}

StepResult Execution::EnterDynamicCall(ActiveFrame& active,
                                       const runtime::DynamicCallPlan& plan,
                                       const runtime::HeapObject& arguments,
                                       const runtime::HeapObject& names,
                                       std::uint8_t destination) {
  const auto callable = plan.method
                            ? std::expected<ResolvedCallable, ExecutionStatus>(ResolvedCallable{
                                  .function = *plan.method,
                                  .implicit = plan.value,
                              })
                            : ResolveCallable(plan.value);
  if (!callable) {
    return std::unexpected(callable.error());
  }
  const auto style = plan.method ? DynamicCallStyle::Method : DynamicCallStyle::Function;
  const auto& function = functions_.subspan(callable->function).front();
  const auto& signature = function.signature;
  if (!signature) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& call_signature = *signature;
  const auto types = function.View().types;
  const auto bound =
      runtime::BindDynamicArguments(call_signature, *callable, arguments, names, style);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  if (auto checked = runtime::CheckDynamicArguments(call_signature, types, arguments, *bound);
      !checked) {
    return checked;
  }
  const auto entered = EnterFrame({.function = callable->function, .destination = destination});
  if (!entered) {
    return std::unexpected(entered.error());
  }
  auto parameters = *entered;
  if (callable->implicit) {
    parameters.front() = *callable->implicit;
    parameters = parameters.subspan(1);
  }
  if (auto copied = CopyDynamicArguments(call_signature, types, arguments, *bound, parameters);
      !copied) {
    return copied;
  }
  active.dynamic_return = true;
  return CompleteCallSetup();
}

StepResult Execution::CopyDynamicArguments(const CallSignature& signature,
                                           std::span<const runtime::TypeConstraint> types,
                                           const runtime::HeapObject& arguments,
                                           const runtime::BoundArguments& bound,
                                           runtime::RegisterView parameters) {
  for (std::size_t index = 0; index < bound.source_indices.size(); ++index) {
    const auto value =
        runtime::ReadDynamicArgument(isolate_.heap(), signature, types, arguments, bound, index);
    if (!value) {
      return std::unexpected(value.error());
    }
    parameters.subspan(index).front() = *value;
  }
  return {};
}

StepResult Execution::ExecuteDynamicCall(ActiveFrame& active, Instruction instruction) {
  const auto input = active.frame.registers().subspan(DecodeC(instruction), 3);
  const auto callable = input.front().Read();
  const auto arguments = input.subspan(1).front().Read();
  const auto names = input.subspan(2).front().Read();
  if (!arguments.IsObject() || !names.IsObject()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return EnterDynamicCall(active, {.value = callable}, *arguments.AsObject(), *names.AsObject(),
                          DecodeA(instruction));
}

std::expected<runtime::Value, ExecutionStatus> Execution::CreateDynamicCallPlan(
    runtime::Value value,
    std::optional<std::uint32_t> target) {
  if (!target) {
    if (const auto callable = ResolveCallable(value); !callable) {
      return std::unexpected(callable.error());
    }
  }
  return runtime::CreateDynamicCallPlan(isolate_.heap(), {.value = value, .method = target});
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
      if (!type) {
        std::unreachable();
      }
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
      const auto* const target = wrapper.callable();
      if (target == nullptr) {
        std::unreachable();
      }
      if (resolving) {
        const auto plan = CreateDynamicCallPlan(receiver, target->function);
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
      return CompleteCallSetup();
    }
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
  if (resolving) {
    const auto plan = CreateDynamicCallPlan(destination.Read());
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
    if (!index) {
      std::unreachable();
    }
    type = &active.function->layouts->types().subspan(*index).front();
  } else if (member.kind == MemberKind::Property && member.setter) {
    const auto& setter = functions_.subspan(*member.setter).front();
    const auto& signature = setter.signature;
    if (!signature) {
      std::unreachable();
    }
    const auto parameter = signature->parameters.front().type;
    type = &setter.layouts->types().subspan(parameter).front();
  } else {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!runtime::Heap::IsValueOfType(value, *type)) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  }
  value = runtime::UnboxDynamicValue(value, *type);
  if (member.kind == MemberKind::Field) {
    static_cast<void>(isolate_.heap().SetField(*receiver.AsObject(), member.target, value));
    values.subspan(DecodeA(instruction)).front() = input.subspan(2).front();
    return {};
  }
  const auto setter = member.setter;
  if (!setter) {
    std::unreachable();
  }
  const auto parameters = EnterFrame({.function = *setter, .destination = DecodeA(instruction)});
  if (!parameters) {
    return std::unexpected(parameters.error());
  }
  parameters->front() = receiver;
  parameters->subspan(1).front() = value;
  active.dynamic_assignment = static_cast<std::uint8_t>(input_register + 2);
  return CompleteCallSetup();
}

StepResult Execution::ExecuteDynamicMethodCall(ActiveFrame& active, Instruction instruction) {
  const auto input = active.frame.registers().subspan(DecodeC(instruction), 3);
  const auto plan_value = input.front().Read();
  const auto arguments = input.subspan(1).front().Read();
  const auto names = input.subspan(2).front().Read();
  if (!arguments.IsObject() || !names.IsObject()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto plan = runtime::ReadDynamicCallPlan(plan_value);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  return EnterDynamicCall(active, *plan, *arguments.AsObject(), *names.AsObject(),
                          DecodeA(instruction));
}

}  // namespace vela::vm::internal
