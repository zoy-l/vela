#include "runtime/native/host_entry.h"

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include <vela/error.h>
#include <vela/handles.h>
#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/host_type.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/native/native_type.h"

namespace vela::runtime {

HostEntry::HostEntry(Target target, HostType signature)
    : target_(std::move(target)), signature_(std::move(signature)) {}

Result<NativeResolvedDeclaration> HostEntry::Resolve() const {
  if (const auto* named = std::get_if<NativeNamedEntry>(&target_)) {
    const auto resolved = named->Resolve();
    if (!resolved) {
      return std::unexpected(Error::FromStatus(resolved.error()));
    }
    return *resolved;
  }
  return std::get<NativeVersionEntry>(target_).Resolve();
}

Result<void> HostEntry::Check(NativeResolvedDeclaration target) const {
  const auto& state = Heap::NativeProgram(*target.state);
  const auto& image = *state.image;
  if (checked_revision_ == image.revision) {
    return {};
  }
  const auto& function = image.function(target.function);
  const auto& actual = state.signatures.at(target.function);
  const auto& expected = *signature_.constraint().signature;
  const auto& types = state.layouts.types;
  if ((function.flags & kNativeExportedFunction) == 0 || actual.hidden_parameters != 0 ||
      actual.parameters.size() != expected.parameters.size() ||
      actual.result.has_value() != expected.result.has_value()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
  }
  for (std::size_t index = 0; index < actual.parameters.size(); ++index) {
    const auto& parameter = actual.parameters[index];
    auto type = types[parameter.type];
    if (parameter.optional) {
      type.nullable = true;
    }
    if (parameter.rest || type != expected.parameters[index].type) {
      return std::unexpected(Error::FromStatus(ExecutionStatus::TypeError));
    }
  }
  if (actual.result && types[*actual.result] != *expected.result) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::TypeError));
  }
  checked_revision_ = image.revision;
  return {};
}

Result<HostEntry> HostEntry::Bind(NativeInstance& instance,
                                  std::string_view name,
                                  NativeType signature) {
  const auto target = instance.FindFunction(name);
  if (!target) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
  }
  auto version = NativeVersionEntry::Bind(instance, *target);
  if (!version) {
    return std::unexpected(Error::FromStatus(version.error()));
  }
  auto type = HostType::Bind(instance, signature);
  if (!type) {
    return std::unexpected(Error::FromStatus(type.error()));
  }
  HostEntry entry(std::move(*version), std::move(*type));
  auto checked = entry.Check({.state = instance.state().front().AsObject(), .function = *target});
  if (!checked) {
    return std::unexpected(std::move(checked.error()));
  }
  return entry;
}

Result<HostEntry> HostEntry::Bind(NativeDispatch& dispatch,
                                  std::string_view module,
                                  std::string_view name,
                                  NativeType signature) {
  auto named = dispatch.FindEntry(module, name);
  if (!named) {
    return std::unexpected(Error::FromStatus(named.error()));
  }
  auto type = HostType::Bind(dispatch, signature);
  if (!type) {
    return std::unexpected(Error::FromStatus(type.error()));
  }
  HostEntry entry(std::move(*named), std::move(*type));
  const auto target = entry.Resolve();
  if (!target) {
    return std::unexpected(target.error());
  }
  auto checked = entry.Check(*target);
  if (!checked) {
    return std::unexpected(std::move(checked.error()));
  }
  return entry;
}

Result<HostEntry> HostEntry::Capture() const {
  if (const auto* named = std::get_if<NativeNamedEntry>(&target_)) {
    auto version = named->Capture();
    if (!version) {
      return std::unexpected(Error::FromStatus(version.error()));
    }
    HostEntry captured(std::move(*version), signature_);
    const auto checked = captured.Check(std::get<NativeVersionEntry>(captured.target_).Resolve());
    if (!checked) {
      return std::unexpected(checked.error());
    }
    return captured;
  }
  return *this;
}

Result<void> HostEntry::Invoke(HandleScope& scope,
                               std::span<const Value> arguments,
                               Value& output) const {
  auto& owner = isolate();
  if (&scope.isolate() != &owner) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  const auto target = Resolve();
  if (!target) {
    return std::unexpected(target.error());
  }
  auto checked = Check(*target);
  if (!checked) {
    return checked;
  }
  auto instance = NativeInstance::Borrow(*target->state);
  auto* parent = owner.native_control().state().host_execution;
  if (parent != nullptr && (&parent->instance().code_identity() != &instance.code_identity())) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
  }
  NativeExecution execution(instance, parent);
  const auto result = InvokeNativeEntry(execution, target->function, arguments);
  if (!result) {
    return std::unexpected(vela::internal::CaptureHostError(execution, result.error()));
  }
  vela::internal::StoreHostResult(owner.heap(), output, *result);
  return {};
}

}  // namespace vela::runtime
