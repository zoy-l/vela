#include "runtime/native/native_function.h"

#include <cassert>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/callable.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/native_binding.h"
#include "base/utf8.h"
#include "runtime/async/future.h"
#include "runtime/async/future_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_type.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_class.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_type.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/types.h"

namespace vela::runtime {
namespace {

bool IsNativeTypeCompatible(const layout::Type& type,
                            NativeType native,
                            const layout::Program& layouts) {
  return native.type && layout::IsSameType(layouts, type, native.layouts(), *native.type);
}

}  // namespace

HandleScope& NativeContext::handles() const noexcept {
  assert(scope_ != nullptr);
  return scope_->handles();
}

HandleScope& NativeContextScope::handles() noexcept {
  if (!handles_) {
    handles_.emplace(context_->isolate());
  }
  return *handles_;
}

std::expected<HostType, ExecutionStatus> NativeContext::BindTypeContract(NativeType type) const {
  return execution_ == nullptr ? HostType::Bind(isolate(), type)
                               : HostType::Bind(execution_->instance(), type);
}

bool NativeFunction::IsSignatureCompatible(const CallSignature& signature,
                                           const layout::Program& layouts) const {
  const auto& types = layouts.types;
  const auto parameters = this->parameters();
  const auto receiver_count = static_cast<std::size_t>(signature.receiver.has_value());
  const auto result_type = this->result_type();
  const bool matches_result =
      signature.result ? IsNativeTypeCompatible(types[*signature.result], result_type, layouts)
                       : result_type.IsVoid();
  if (signature.parameters.size() + receiver_count != parameters.size() || !matches_result) {
    return false;
  }
  if (signature.receiver &&
      !IsNativeTypeCompatible(types[*signature.receiver], parameters.front(), layouts)) {
    return false;
  }
  if (member_ != nullptr) {
    if (member_->parameter_names.size() != signature.parameters.size() ||
        HasNativeReceiver(member_->kind) != signature.receiver.has_value()) {
      return false;
    }
  }
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& parameter = signature.parameters[index];
    if (member_ != nullptr && (member_->parameter_names[index] != parameter.name ||
                               parameter.optional || parameter.rest)) {
      return false;
    }
    if (!IsNativeTypeCompatible(types[parameter.type], parameters[index + receiver_count],
                                layouts)) {
      return false;
    }
  }
  return true;
}

NativeResult NativeContext::String(std::string_view text) const {
  if (InvalidUtf8Offset(text)) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::NativeFailure,
        .message = "native function returned invalid UTF-8",
        .stack_trace = {},
    });
  }
  auto* string = isolate_->heap().AllocateString(text);
  if (string == nullptr) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::OutOfMemory,
        .message = {},
        .stack_trace = {},
    });
  }
  return Value::String(string);
}

Result<Local<vela::Value>> NativeContext::TypedFutureValue(NativeType type,
                                                           const Future& future) const {
  auto& scope = handles();
  auto bound = future.Rooted();
  if (!bound.Bind(isolate_->scheduler(), isolate_->heap())) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::TypeError,
        .message = "native Future belongs to a different Isolate",
        .stack_trace = {},
    });
  }
  auto* object = isolate_->heap().AllocateFuture(std::move(bound));
  if (object == nullptr) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::OutOfMemory,
        .message = {},
        .stack_trace = {},
    });
  }
  const auto raw = Value::Object(object);
  auto rooted = vela::internal::HandleAccess::Root<vela::Value>(scope, raw);
  if (!rooted) {
    return std::unexpected(std::move(rooted.error()));
  }
  const auto binding = BindTypeContract(type);
  if (!binding) {
    return std::unexpected(
        vela::Error{.status = binding.error(), .message = {}, .stack_trace = {}});
  }
  const auto constraint = binding->constraint();
  constexpr std::string_view kCompletionMessage =
      "native Future completion does not match its type";
  const auto diagnostic =
      execution_ == nullptr
          ? FutureDiagnostic{.message = std::string(kCompletionMessage),
                             .stack_trace = {},
                             .status = ExecutionStatus::TypeError,
                             .origin = {},}
          : execution_->Diagnostic(ExecutionStatus::TypeError, kCompletionMessage);
  const auto checked =
      CheckFutureCompletion(isolate(), raw, constraint.signature->result, diagnostic, {});
  if (!checked) {
    return std::unexpected(
        vela::Error{.status = checked.error(), .message = {}, .stack_trace = {}});
  }
  const auto typed = Heap::BindRuntimeType(*checked->AsObject(), constraint);
  if (!typed) {
    return std::unexpected(vela::Error{.status = typed.error(), .message = {}, .stack_trace = {}});
  }
  return vela::internal::HandleAccess::Root<vela::Value>(scope, *checked);
}

Completer NativeContext::CreateCompleter() const {
  return isolate_->scheduler().CreateHostCompleter();
}

NativeResult NativeContext::CancellationValue(const CancellationToken& token) const {
  auto* object = isolate_->heap().AllocateCancellationToken(token);
  if (object == nullptr) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::OutOfMemory,
        .message = {},
        .stack_trace = {},
    });
  }
  return Value::Object(object);
}

CancellationToken internal::NativeCodec<CancellationToken>::Decode(Value value) {
  return *Heap::GetCancellationToken(*value.AsObject());
}

NativeResult internal::NativeCodec<CancellationToken>::Encode(NativeContext& context,
                                                              const CancellationToken& token) {
  return context.CancellationValue(token);
}

std::string_view internal::NativeCodec<std::string_view>::Decode(Value value) {
  return value.AsString()->value();
}

NativeResult internal::ValidateNativeResult(NativeResult result) {
  if (!result && (result.error().status == ExecutionStatus::OutOfMemory ||
                  result.error().status == ExecutionStatus::ExecutionStopped)) {
    return result;
  }
  if (!result && InvalidUtf8Offset(result.error().message)) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::NativeFailure,
        .message = "native function returned an invalid UTF-8 error",
        .stack_trace = {},
    });
  }
  return result;
}

}  // namespace vela::runtime
