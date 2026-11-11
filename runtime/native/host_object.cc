#include "runtime/native/host_object.h"
#include <optional>

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/host_type.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_dynamic.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/native/native_type.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_buffer.h"
#include "runtime/value/value_operations.h"

namespace vela::internal {
namespace {

Result<runtime::HeapObject*> ResolveHostProgram(HandleScope& scope, const runtime::HostType& type) {
  if (&type.isolate() != &scope.isolate()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  const auto program = type.program();
  if (!program) {
    return std::unexpected(Error::FromStatus(program.error()));
  }
  auto* parent = scope.isolate().native_control().state().host_execution;
  const auto& state = runtime::Heap::NativeProgram(**program);
  if (!runtime::CanReenterNativeProgram(parent, *state.code_identity)) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
  }
  return *program;
}

Result<runtime::HeapObject*> ResolveMemberProgram(HandleScope& scope,
                                                  const runtime::HostType& type,
                                                  runtime::Value receiver,
                                                  const runtime::TypeConstraint& contract) {
  if (!runtime::Heap::IsValueOfType(receiver, contract)) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidCast));
  }
  return ResolveHostProgram(scope, type);
}

}  // namespace

Result<void> ConstructHostObject(HandleScope& scope,
                                 const runtime::HostType& type,
                                 std::span<const HostArgument> arguments,
                                 runtime::Value& output) {
  const auto program = ResolveHostProgram(scope, type);
  if (!program) {
    return std::unexpected(program.error());
  }
  auto instance = runtime::NativeInstance::Borrow(**program);
  const auto& state = runtime::Heap::NativeProgram(**program);
  const auto found = state.class_indices.find(type.constraint().nominal);
  if (found == state.class_indices.end()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
  }
  const auto target = instance.image().classes.values()[found->second].constructor;
  if (target == runtime::kNoNativeIndex) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
  }
  const auto& signature = instance.call_signature(target);
  const auto count = signature.parameters.size() / 2;
  runtime::ValueBuffer storage(signature.parameters.size());
  if (!storage.IsAllocated()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::OutOfMemory));
  }
  const auto values = storage.values();
  for (std::size_t index = 0; index < count; ++index) {
    values[index + count] = runtime::Value::Boolean(false);
  }
  for (const auto& argument : arguments) {
    const auto parameter = signature.ParameterIndex(argument.name);
    if (!parameter || *parameter >= count || values[*parameter + count].AsBoolean()) {
      return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
    }
    values[*parameter] = argument.value;
    values[*parameter + count] = runtime::Value::Boolean(true);
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (!signature.parameters[index].optional && !values[index + count].AsBoolean()) {
      return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
    }
  }
  runtime::NativeExecution execution(instance,
                                     scope.isolate().native_control().state().host_execution);
  const auto result = runtime::InvokeNativeFunction(execution, target, values);
  if (!result) {
    return std::unexpected(CaptureHostError(execution, result.error()));
  }
  StoreHostResult(scope.isolate().heap(), output, *result);
  return {};
}

Result<void> ReadHostEnum(HandleScope& scope,
                          const runtime::HostType& type,
                          std::string_view name,
                          runtime::Value& output) {
  const auto program = ResolveHostProgram(scope, type);
  if (!program) {
    return std::unexpected(program.error());
  }
  const auto contract = type.constraint();
  if (contract.nominal == nullptr || !contract.nominal->IsEnum()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::TypeError));
  }
  const auto& state = runtime::Heap::NativeProgram(**program);
  const auto member = state.enum_members.find(std::pair(contract.nominal, name));
  if (member == state.enum_members.end()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidField));
  }
  const auto ordinal = runtime::Value::Integer(member->second);
  const auto value = runtime::BoxDynamicValue(scope.isolate().heap(), ordinal, contract);
  if (!value) {
    return std::unexpected(Error::FromStatus(value.error()));
  }
  StoreHostResult(scope.isolate().heap(), output, *value);
  return {};
}

Result<void> ReadHostMember(HandleScope& scope,
                            const runtime::HostType& type,
                            runtime::Value receiver,
                            std::string_view name,
                            runtime::NativeType result_type,
                            runtime::Value& output) {
  const auto contract = type.constraint();
  const auto program = ResolveMemberProgram(scope, type, receiver, contract);
  if (!program) {
    return std::unexpected(program.error());
  }
  auto instance = runtime::NativeInstance::Borrow(**program);
  auto expected = runtime::ResolveInstanceNativeType(instance, result_type);
  if (!expected) {
    return std::unexpected(Error::FromStatus(expected.error()));
  }
  const runtime::RootRange result_type_root(scope.isolate().heap(), std::span(&*expected, 1),
                                            std::nullopt);
  runtime::NativeExecution execution(instance,
                                     scope.isolate().native_control().state().host_execution);
  const auto result = runtime::ReadNativeMember(execution, receiver, name, contract);
  if (!result) {
    return std::unexpected(CaptureHostError(execution, result.error()));
  }
  return CastHostValue(scope, *result, *expected, output);
}

Result<void> WriteHostMember(HandleScope& scope,
                             const runtime::HostType& type,
                             runtime::Value receiver,
                             std::string_view name,
                             runtime::Value value) {
  const auto contract = type.constraint();
  const auto program = ResolveMemberProgram(scope, type, receiver, contract);
  if (!program) {
    return std::unexpected(program.error());
  }
  auto instance = runtime::NativeInstance::Borrow(**program);
  runtime::NativeExecution execution(instance,
                                     scope.isolate().native_control().state().host_execution);
  const auto result = runtime::WriteNativeMember(execution, receiver, name, value, contract);
  if (!result) {
    return std::unexpected(CaptureHostError(execution, result.error()));
  }
  return {};
}

}  // namespace vela::internal
