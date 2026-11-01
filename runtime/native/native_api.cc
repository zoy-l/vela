#include "runtime/native/native_api.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/execution_status.h"
#include "base/utf8.h"
#include "runtime/async/future.h"
#include "runtime/exceptions/error.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_buffer.h"

struct VelaAotHandle final {
  VelaAotHandle(vela::runtime::Isolate& owner,
                vela::runtime::Value source,
                vela::runtime::FutureDiagnostic detail)
      : isolate(&owner),
        value(source),
        root(owner.heap(), &value, 1, vela::runtime::RootOwner::NativeHandle),
        diagnostic(std::move(detail)) {}
  vela::runtime::Isolate* isolate;
  vela::runtime::Value value;
  vela::runtime::RootRange root;
  vela::runtime::FutureDiagnostic diagnostic;
};

namespace vela::runtime {
namespace {

static_assert(sizeof(VelaAotValue) == kNativeValueBytes);
static_assert(static_cast<VelaAotStatus>(ExecutionStatus::Returned) == VELA_AOT_RETURNED);

std::expected<VelaAotHandle*, ExecutionStatus> MakeHandle(Isolate& isolate,
                                                          Value value,
                                                          FutureDiagnostic diagnostic = {}) {
  std::unique_ptr<VelaAotHandle> owned(new (std::nothrow)
                                           VelaAotHandle(isolate, value, std::move(diagnostic)));
  if (owned == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return owned.release();
}

std::expected<VelaAotValue, ExecutionStatus> EncodePublic(Isolate& isolate, Value value) {
  VelaAotValue result{};
  if (value.IsReference()) {
    const auto handle = MakeHandle(isolate, value);
    if (!handle) {
      return std::unexpected(handle.error());
    }
    result.kind = VELA_AOT_HANDLE;
    result.payload = std::bit_cast<std::uint64_t>(*handle);
  } else if (value.IsInteger()) {
    result.kind = VELA_AOT_INTEGER;
    result.payload = std::bit_cast<std::uint64_t>(value.AsInteger());
  } else if (value.IsDouble()) {
    result.kind = VELA_AOT_DOUBLE;
    result.payload = std::bit_cast<std::uint64_t>(value.AsDouble());
  } else if (value.IsBoolean()) {
    result.kind = VELA_AOT_BOOLEAN;
    result.payload = value.AsBoolean() ? 1U : 0U;
  }
  return result;
}

std::expected<Value, ExecutionStatus> DecodePublic(Isolate& isolate, const VelaAotValue& input) {
  if (input.reserved_byte != 0 || input.reserved_half != 0 || input.reserved_word != 0) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  switch (input.kind) {
    case VELA_AOT_NULL:
      return input.payload == 0 ? std::expected<Value, ExecutionStatus>(Value::Null())
                                : std::unexpected(ExecutionStatus::TypeError);
    case VELA_AOT_BOOLEAN:
      return input.payload <= 1
                 ? std::expected<Value, ExecutionStatus>(Value::Boolean(input.payload != 0))
                 : std::unexpected(ExecutionStatus::TypeError);
    case VELA_AOT_INTEGER:
      return Value::Integer(std::bit_cast<std::int64_t>(input.payload));
    case VELA_AOT_DOUBLE:
      return Value::Double(std::bit_cast<double>(input.payload));
    case VELA_AOT_HANDLE: {
      const auto* handle = std::bit_cast<const VelaAotHandle*>(input.payload);
      if (handle == nullptr) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      return handle->isolate == &isolate ? std::expected<Value, ExecutionStatus>(handle->value)
                                         : std::unexpected(ExecutionStatus::InvalidIsolate);
    }
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
}

VelaAotStatus StoreError(Isolate& isolate,
                         ExecutionStatus status,
                         Value value,
                         FutureDiagnostic diagnostic,
                         VelaAotHandle** error) {
  if (error == nullptr) {
    return ToNativeStatus(status);
  }
  if (diagnostic.message.empty()) {
    diagnostic.message = ExecutionStatusName(status);
  }
  diagnostic.status = status;
  if (value.IsNull() && status != ExecutionStatus::OutOfMemory) {
    const auto created =
        CreateError(isolate.heap(), isolate.error_layout(),
                    {.message = diagnostic.message, .code = ExecutionStatusCode(status)});
    if (!created) {
      return ToNativeStatus(created.error());
    }
    value = *created;
  }
  const auto owned = MakeHandle(isolate, value, std::move(diagnostic));
  if (!owned) {
    return ToNativeStatus(owned.error());
  }
  *error = *owned;
  return ToNativeStatus(status);
}

VelaAotStatus StorePublic(Isolate& isolate, Value value, VelaAotValue* output) {
  const auto encoded = EncodePublic(isolate, value);
  if (!encoded) {
    return ToNativeStatus(encoded.error());
  }
  *output = *encoded;
  return VELA_AOT_RETURNED;
}

std::expected<Value, ExecutionStatus> Invoke(NativeExecution& execution,
                                             std::uint32_t function,
                                             std::span<const VelaAotValue> input) {
  ValueBuffer storage(input.size());
  if (!storage.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  auto& isolate = execution.isolate();
  const auto values = storage.values();
  const RootRange roots(isolate.heap(), values.data(), values.size(), RootOwner::Runtime);
  for (std::size_t index = 0; index < input.size(); ++index) {
    const auto value = DecodePublic(isolate, input.subspan(index).front());
    if (!value) {
      return std::unexpected(value.error());
    }
    values.subspan(index).front() = *value;
  }
  return InvokeNativeEntry(execution, function, values);
}

}  // namespace

extern "C" VelaAotIsolate* vela_aot_v1_isolate_create() {
  std::unique_ptr<Isolate> isolate(new (std::nothrow) Isolate);
  return isolate.release();
}

extern "C" void vela_aot_v1_isolate_release(VelaAotIsolate* isolate) {
  const std::unique_ptr<Isolate> owned(isolate);
}

extern "C" VelaAotStatus vela_aot_v1_instantiate(VelaAotIsolate* owner,
                                                 const void* image,
                                                 VelaAotInstance** instance,
                                                 VelaAotHandle** error) {
  if (owner == nullptr || !owner->scheduler().IsOwnerThread()) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (instance == nullptr || image == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  *instance = nullptr;
  if (error != nullptr) {
    *error = nullptr;
  }
  auto created = NativeInstance::Instantiate(*owner, *static_cast<const NativeImage*>(image));
  if (!created) {
    const auto& failure = created.error();
    const auto status = failure.status == NativeInstantiationStatus::OutOfMemory
                            ? ExecutionStatus::OutOfMemory
                            : ExecutionStatus::InvalidEntry;
    return StoreError(
        *owner, status, Value::Null(),
        {.message = failure.message, .stack_trace = {}, .status = status, .origin = {}}, error);
  }
  *instance = created->release();
  return VELA_AOT_RETURNED;
}

extern "C" void vela_aot_v1_instance_release(VelaAotInstance* instance) {
  const std::unique_ptr<NativeInstance> owned(instance);
}

extern "C" VelaAotStatus vela_aot_v1_invoke(VelaAotInstance* native,
                                            const void* image,
                                            std::uint32_t function,
                                            const VelaAotValue* arguments,
                                            std::uint64_t count,
                                            VelaAotValue* result,
                                            VelaAotHandle** error) {
  if (native == nullptr || !native->isolate().scheduler().IsOwnerThread()) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (image != &native->image() || result == nullptr || (count != 0 && arguments == nullptr)) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  if (error != nullptr) {
    *error = nullptr;
  }
  NativeExecution execution(*native);
  const auto invoked = Invoke(execution, function, {arguments, count});
  if (invoked) {
    return StorePublic(native->isolate(), *invoked, result);
  }
  const auto status = invoked.error();
  return StoreError(native->isolate(), status, execution.ExceptionPayload(),
                    execution.FailureDiagnostic(status), error);
}

extern "C" VelaAotStatus vela_aot_v1_string_create(VelaAotIsolate* owner,
                                                   const char* text,
                                                   std::uint64_t size,
                                                   VelaAotHandle** result) {
  if (owner == nullptr || !owner->scheduler().IsOwnerThread()) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (result == nullptr || (text == nullptr && size != 0)) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  *result = nullptr;
  const std::string_view source(text != nullptr ? text : "", size);
  if (InvalidUtf8Offset(source)) {
    return ToNativeStatus(ExecutionStatus::TypeError);
  }
  auto* string = owner->heap().AllocateString(source);
  if (string == nullptr) {
    return ToNativeStatus(ExecutionStatus::OutOfMemory);
  }
  const auto owned = MakeHandle(*owner, Value::String(string));
  if (!owned) {
    return ToNativeStatus(owned.error());
  }
  *result = *owned;
  return VELA_AOT_RETURNED;
}

extern "C" VelaAotStatus vela_aot_v1_string_view(const VelaAotHandle* handle,
                                                 const char** text,
                                                 std::uint64_t* size) {
  if (handle == nullptr || text == nullptr || size == nullptr || !handle->value.IsString()) {
    return ToNativeStatus(ExecutionStatus::TypeError);
  }
  const auto value = handle->value.AsString()->value();
  *text = value.data();
  *size = value.size();
  return VELA_AOT_RETURNED;
}

extern "C" void vela_aot_v1_handle_release(VelaAotHandle* handle) {
  const std::unique_ptr<VelaAotHandle> owned(handle);
}

extern "C" VelaAotStatus vela_aot_v1_future_wait(VelaAotHandle* future,
                                                 std::uint8_t wait_for_host,
                                                 VelaAotValue* result,
                                                 VelaAotHandle** error) {
  if (future == nullptr || result == nullptr || wait_for_host > 1 || !future->value.IsObject()) {
    return ToNativeStatus(ExecutionStatus::TypeError);
  }
  auto& isolate = *future->isolate;
  if (!isolate.scheduler().IsOwnerThread()) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (error != nullptr) {
    *error = nullptr;
  }
  auto* value = Heap::GetFuture(*future->value.AsObject());
  if (value == nullptr) {
    return ToNativeStatus(ExecutionStatus::TypeError);
  }
  value->MarkObserved();
  const auto mode =
      wait_for_host != 0 ? FuturePumpMode::WaitForHost : FuturePumpMode::AvailableWork;
  if (isolate.scheduler().PumpUntil(*value, mode) == FuturePumpResult::Idle) {
    return ToNativeStatus(ExecutionStatus::AsyncIdle);
  }
  if (const auto completed = value->value()) {
    return StorePublic(isolate, *completed, result);
  }
  const auto diagnostic = value->error_diagnostic();
  const auto payload = value->error();
  if (!diagnostic || !payload) {
    std::unreachable();
  }
  return StoreError(isolate, diagnostic->status, *payload, *diagnostic, error);
}

extern "C" const char* vela_aot_v1_status_code(VelaAotStatus status) {
  return status <= ToNativeStatus(ExecutionStatus::ExecutionStopped)
             ? ExecutionStatusCode(static_cast<ExecutionStatus>(status)).data()
             : "InvalidStatus";
}

extern "C" VelaAotStatus vela_aot_v1_error_message(const VelaAotHandle* error,
                                                   const char** text,
                                                   std::uint64_t* size) {
  if (error == nullptr || text == nullptr || size == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  const auto& message = error->diagnostic.message;
  *text = message.data();
  *size = message.size();
  return VELA_AOT_RETURNED;
}

extern "C" VelaAotStatus vela_aot_v1_error_frame(const VelaAotHandle* error,
                                                 std::uint64_t index,
                                                 VelaAotTraceFrame* frame) {
  if (error == nullptr || frame == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  const auto& trace = error->diagnostic.stack_trace;
  if (index >= trace.size()) {
    return ToNativeStatus(ExecutionStatus::IndexOutOfBounds);
  }
  const auto& source = trace.at(index);
  *frame = {
      .function = source.function.data(),
      .function_size = source.function.size(),
      .source_path = source.source_path.data(),
      .source_path_size = source.source_path.size(),
      .instruction = source.instruction,
      .line = source.line,
      .column = source.column,
  };
  return VELA_AOT_RETURNED;
}

}  // namespace vela::runtime
