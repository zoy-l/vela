#include "runtime/native_function.h"
#include "runtime/future.h"

#include <expected>
#include <string_view>
#include <utility>

#include "base/utf8.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/register_storage.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
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

bool internal::NativeCodec<CancellationToken>::IsCompatible(RegisterReference value) {
  return value.IsObject() && value.AsObject() != nullptr &&
         Heap::GetCancellationToken(*value.AsObject()) != nullptr;
}

CancellationToken internal::NativeCodec<CancellationToken>::Decode(RegisterReference value) {
  return *Heap::GetCancellationToken(*value.AsObject());
}

NativeResult internal::NativeCodec<CancellationToken>::Encode(NativeContext& context,
                                                              const CancellationToken& token) {
  return context.CancellationValue(token);
}

std::string_view internal::NativeCodec<std::string_view>::Decode(RegisterReference value) {
  return value.AsString()->value();
}

bool internal::NativeCodec<Future>::IsCompatible(RegisterReference value) {
  return value.IsObject() && value.AsObject() != nullptr &&
         Heap::GetFuture(*value.AsObject()) != nullptr;
}

Future internal::NativeCodec<Future>::Decode(RegisterReference value) {
  return Heap::GetFuture(*value.AsObject())->Rooted();
}

NativeResult internal::NativeCodec<Future>::Encode(NativeContext& context, const Future& future) {
  return context.FutureValue(future);
}

NativeResult NativeFunction::Invoke(Isolate& isolate, RegisterView arguments) const {
  NativeContext context(isolate);
  auto result = callback_(context, arguments);
  if (!result && InvalidUtf8Offset(result.error().message)) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::InvalidUtf8,
        .message = "native function returned an invalid UTF-8 error",
    });
  }
  return result;
}

}  // namespace vela::runtime
