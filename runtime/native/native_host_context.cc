#include "runtime/native/native_context.h"

#include <cstddef>
#include <expected>
#include <limits>
#include <utility>

#include "runtime/heap/heap.h"
#include "runtime/heap/host_memory.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_class.h"
#include "runtime/native/native_type.h"
#include "runtime/value/host_object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

NativeError HostError(HostObjectError error) {
  switch (error) {
    case HostObjectError::OutOfMemory:
      return {
          .kind = NativeErrorKind::OutOfMemory,
          .message = "out of memory wrapping native object",
      };
    case HostObjectError::OwnershipConflict:
      return {
          .kind = NativeErrorKind::TypeMismatch,
          .message = "conflicting native object ownership",
      };
    case HostObjectError::InvalidSharedOwner:
      return {
          .kind = NativeErrorKind::TypeMismatch,
          .message = "native shared pointer has no lifetime owner",
      };
    case HostObjectError::ForeignIsolate:
      return {
          .kind = NativeErrorKind::TypeMismatch,
          .message = "native construction belongs to another Isolate",
      };
  }
  std::unreachable();
}

}  // namespace

HostMemory NativeContext::ReserveHostMemory(std::size_t bytes) const noexcept {
  return {isolate_->heap(), bytes};
}

NativeResult NativeContext::PublishHost(NativeType type, HostObjectStorage storage) const {
  if (storage.pointer() == nullptr) {
    if (type.nullable) {
      return Value::Null();
    }
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::TypeMismatch,
        .message = "native construction returned null",
    });
  }
  const auto* binding = isolate_->FindNativeClass(type.host_key);
  if (binding == nullptr || binding->description->type.cpp_identity != type.cpp_identity) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::TypeMismatch,
        .message = "native result has an unregistered host type",
    });
  }
  auto wrapped = isolate_->heap().WrapHostObject(*binding->layout, binding->description->size,
                                                 std::move(storage));
  if (!wrapped) {
    return std::unexpected(HostError(wrapped.error()));
  }
  return Value::Object(*wrapped);
}

std::expected<void, NativeError> NativeContext::SetHostMemory(NativeType type,
                                                              const void* object,
                                                              std::size_t extra_bytes) const {
  const auto* binding = isolate_->FindNativeClass(type.host_key);
  if (binding == nullptr || binding->description->type.cpp_identity != type.cpp_identity) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::TypeMismatch,
        .message = "native memory report has an unregistered host type",
    });
  }
  const auto size = binding->description->size;
  if (extra_bytes > std::numeric_limits<std::size_t>::max() - size) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::OutOfMemory,
        .message = "native memory report exceeds heap budget",
    });
  }
  auto& heap = isolate_->heap();
  auto* wrapper = heap.FindHostObject(binding->layout->nominal_type(), object);
  if (wrapper == nullptr) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::TypeMismatch,
        .message = "native memory report requires a live wrapper",
    });
  }
  if (!heap.SetHostMemory(*wrapper, size + extra_bytes)) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::OutOfMemory,
        .message = "native memory report exceeds heap budget",
    });
  }
  return {};
}

}  // namespace vela::runtime
