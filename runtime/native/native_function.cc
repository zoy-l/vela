#include "runtime/native/native_function.h"
#include "runtime/async/future.h"

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include "base/callable.h"
#include "base/layout_description.h"
#include "base/utf8.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "runtime/strings/string.h"
#include "runtime/value/register_storage.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

bool IsNativeTypeCompatible(const layout::Type& type, NativeType native) {
  using Kind = layout::Type::Kind;
  if (native == NativeType::NullableString) {
    return type.kind == Kind::String && type.nullable;
  }
  if (type.nullable) {
    return false;
  }
  switch (native) {
    case NativeType::Void:
      return false;
    case NativeType::Boolean:
      return type.kind == Kind::Boolean;
    case NativeType::Integer:
      return type.kind == Kind::Integer;
    case NativeType::Double:
      return type.kind == Kind::Double;
    case NativeType::String:
      return type.kind == Kind::String;
    case NativeType::NullableString:
      std::unreachable();
    case NativeType::Future:
      return type.kind == Kind::Future;
    case NativeType::CancellationToken:
      return type.kind == Kind::CancellationToken;
  }
  std::unreachable();
}

}  // namespace

bool NativeFunction::IsSignatureCompatible(const CallSignature& signature,
                                           std::span<const layout::Type> types) const {
  const auto parameters = this->parameters();
  const auto result_type = this->result_type();
  const bool matches_result =
      signature.result
          ? IsNativeTypeCompatible(types.subspan(*signature.result).front(), result_type)
          : result_type == NativeType::Void;
  if (signature.parameters.size() != parameters.size() || !matches_result) {
    return false;
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (!IsNativeTypeCompatible(types.subspan(signature.parameters.at(index).type).front(),
                                parameters.subspan(index).front())) {
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

bool internal::NativeCodec<CancellationToken>::IsCompatible(Value value) {
  return value.IsObject() && value.AsObject() != nullptr &&
         Heap::GetCancellationToken(*value.AsObject()) != nullptr;
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

bool internal::NativeCodec<Future>::IsCompatible(Value value) {
  return value.IsObject() && value.AsObject() != nullptr &&
         Heap::GetFuture(*value.AsObject()) != nullptr;
}

Future internal::NativeCodec<Future>::Decode(Value value) {
  return Heap::GetFuture(*value.AsObject())->Rooted();
}

NativeResult internal::NativeCodec<Future>::Encode(NativeContext& context, const Future& future) {
  return context.FutureValue(future);
}

NativeResult NativeFunction::Invoke(Isolate& isolate, RegisterView arguments) const {
  NativeContext context(isolate);
  return internal::ValidateNativeResult(callback_(context, arguments));
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
