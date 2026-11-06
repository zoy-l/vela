#include "base/execution_status.h"
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
#include "vela/error.h"

namespace vela::runtime {
namespace {

const NativeClassBinding* FindHostBinding(Isolate& isolate, NativeType type) {
  const auto* binding = isolate.FindNativeClass(type.host_key());
  return binding != nullptr && binding->description->type.cpp_identity() == type.cpp_identity()
             ? binding
             : nullptr;
}

vela::Error HostError(HostObjectError error) {
  switch (error) {
    case HostObjectError::OutOfMemory:
      return {
          .status = vela::ExecutionStatus::OutOfMemory,
          .message = {},
          .stack_trace = {},
      };
    case HostObjectError::OwnershipConflict:
      return {
          .status = vela::ExecutionStatus::TypeError,
          .message = "conflicting native object ownership",
          .stack_trace = {},
      };
    case HostObjectError::InvalidSharedOwner:
      return {
          .status = vela::ExecutionStatus::TypeError,
          .message = "native shared pointer has no lifetime owner",
          .stack_trace = {},
      };
    case HostObjectError::ForeignIsolate:
      return {
          .status = vela::ExecutionStatus::TypeError,
          .message = "native construction belongs to another Isolate",
          .stack_trace = {},
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
    if (type.IsNullable()) {
      return Value::Null();
    }
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::TypeError,
        .message = "native construction returned null",
        .stack_trace = {},
    });
  }
  const auto* binding = FindHostBinding(*isolate_, type);
  if (binding == nullptr) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::TypeError,
        .message = "native result has an unregistered host type",
        .stack_trace = {},
    });
  }
  auto wrapped = isolate_->heap().WrapHostObject(*binding->layout, binding->description->size,
                                                 std::move(storage));
  if (!wrapped) {
    return std::unexpected(HostError(wrapped.error()));
  }
  return Value::Object(*wrapped);
}

std::expected<void, vela::Error> NativeContext::SetHostMemory(NativeType type,
                                                              const void* object,
                                                              std::size_t extra_bytes) const {
  const auto* binding = FindHostBinding(*isolate_, type);
  if (binding == nullptr) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::TypeError,
        .message = "native memory report has an unregistered host type",
        .stack_trace = {},
    });
  }
  const auto size = binding->description->size;
  if (extra_bytes > std::numeric_limits<std::size_t>::max() - size) {
    return std::unexpected(vela::Error::FromStatus(vela::ExecutionStatus::OutOfMemory));
  }
  auto& heap = isolate_->heap();
  auto* wrapper = heap.FindHostObject(binding->layout->nominal_type(), object);
  if (wrapper == nullptr) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::TypeError,
        .message = "native memory report requires a live wrapper",
        .stack_trace = {},
    });
  }
  if (!heap.SetHostMemory(*wrapper, size + extra_bytes)) {
    return std::unexpected(vela::Error::FromStatus(vela::ExecutionStatus::OutOfMemory));
  }
  return {};
}

}  // namespace vela::runtime
