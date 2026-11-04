#include "runtime/native/native_function.h"

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include "base/callable.h"
#include "base/layout_description.h"
#include "base/native_binding.h"
#include "base/utf8.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_class.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_type.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

bool IsNativeTypeCompatible(const layout::Type& type,
                            NativeType native,
                            std::span<const layout::Class> classes) {
  using Kind = layout::Type::Kind;
  if (type.nullable != native.nullable) {
    return false;
  }
  switch (native.kind) {
    case NativeKind::Void:
      return false;
    case NativeKind::Boolean:
      return type.kind == Kind::Boolean;
    case NativeKind::Integer:
      return type.kind == Kind::Integer;
    case NativeKind::Double:
      return type.kind == Kind::Double;
    case NativeKind::String:
      return type.kind == Kind::String;
    case NativeKind::Future:
      return type.kind == Kind::Future;
    case NativeKind::CancellationToken:
      return type.kind == Kind::CancellationToken;
    case NativeKind::Host:
      return type.kind == Kind::Instance && classes[type.index].native_key == native.host_key;
  }
  std::unreachable();
}

}  // namespace

bool NativeFunction::IsSignatureCompatible(const CallSignature& signature,
                                           std::span<const layout::Type> types,
                                           std::span<const layout::Class> classes) const {
  const auto parameters = this->parameters();
  const auto receiver_count = static_cast<std::size_t>(signature.receiver.has_value());
  const auto result_type = this->result_type();
  const bool matches_result =
      signature.result
          ? IsNativeTypeCompatible(types.subspan(*signature.result).front(), result_type, classes)
          : result_type.kind == NativeKind::Void;
  if (signature.parameters.size() + receiver_count != parameters.size() || !matches_result) {
    return false;
  }
  if (signature.receiver &&
      !IsNativeTypeCompatible(types[*signature.receiver], parameters.front(), classes)) {
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
                                classes)) {
      return false;
    }
  }
  return true;
}

NativeResult NativeContext::String(std::string_view text) const {
  if (InvalidUtf8Offset(text)) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::InvalidUtf8,
        .message = "native function returned invalid UTF-8",
    });
  }
  auto* string = isolate_->heap().AllocateString(text);
  if (string == nullptr) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::OutOfMemory,
        .message = "out of memory creating native string",
    });
  }
  return Value::String(string);
}

std::expected<Value, NativeError> NativeContext::FutureValue(const Future& future) const {
  auto bound = future.Rooted();
  if (!bound.Bind(isolate_->scheduler(), isolate_->heap())) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::TypeMismatch,
        .message = "native Future belongs to a different Isolate",
    });
  }
  auto* object = isolate_->heap().AllocateFuture(std::move(bound));
  if (object == nullptr) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::OutOfMemory,
        .message = "out of memory creating native Future",
    });
  }
  return Value::Object(object);
}

Completer NativeContext::CreateCompleter() const {
  return isolate_->scheduler().CreateHostCompleter();
}

NativeResult NativeContext::CancellationValue(const CancellationToken& token) const {
  auto* object = isolate_->heap().AllocateCancellationToken(token);
  if (object == nullptr) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::OutOfMemory,
        .message = "out of memory creating CancellationToken",
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

Future internal::NativeCodec<Future>::Decode(Value value) {
  return Heap::GetFuture(*value.AsObject())->Rooted();
}

NativeResult internal::NativeCodec<Future>::Encode(NativeContext& context, const Future& future) {
  return context.FutureValue(future);
}

NativeResult internal::ValidateNativeResult(NativeResult result) {
  if (!result && InvalidUtf8Offset(result.error().message)) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::InvalidUtf8,
        .message = "native function returned an invalid UTF-8 error",
    });
  }
  return result;
}

}  // namespace vela::runtime
