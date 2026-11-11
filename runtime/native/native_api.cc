#include "runtime/native/native_api.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/host.h>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_async.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_api_cpp.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

struct VelaAotPersistent final {
  vela::runtime::HostRoot root;
};

struct VelaAotError final {
  vela::Error detail;
};

namespace vela::runtime {
namespace {

bool IsOwner(const VelaAotScope* scope) {
  return scope != nullptr && scope->isolate().scheduler().IsOwnerThread();
}

Result<Value> ReadSlot(VelaAotLocal local) {
  if (local.slot == nullptr) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
  }
  return *static_cast<const Value*>(local.slot);
}

Result<Value> ReadLocal(VelaAotLocal local) {
  if (!IsOwner(local.scope)) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return ReadSlot(local);
}

VelaAotStatus StoreError(Error detail, VelaAotError** output) {
  auto status = ToNativeStatus(detail.status);
  if (output != nullptr) {
    std::unique_ptr<VelaAotError> owned(new (std::nothrow) VelaAotError{std::move(detail)});
    *output = owned.release();
    if (*output == nullptr) {
      status = ToNativeStatus(ExecutionStatus::OutOfMemory);
    }
  }
  return status;
}

VelaAotStatus StorePublic(HandleScope& scope, Value value, VelaAotValue& output) {
  auto encoded = aot::Encode(scope, value);
  if (!encoded) {
    return ToNativeStatus(encoded.error().status);
  }
  output = *encoded;
  return VELA_AOT_RETURNED;
}

Result<Value> Invoke(NativeExecution& execution,
                     HandleScope& scope,
                     std::uint32_t function,
                     std::span<const VelaAotValue> input) {
  constexpr std::size_t kInlineArguments = 8;
  NativeBuffer<Value, kInlineArguments> storage(scope.isolate().heap(), input.size());
  if (!storage.IsAllocated()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::OutOfMemory));
  }
  const auto values = storage.values();
  for (std::size_t index = 0; index < input.size(); ++index) {
    auto value = aot::Decode(scope, input[index]);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    values[index] = *value;
  }
  const auto invoked = InvokeNativeEntry(execution, function, values);
  if (!invoked) {
    return std::unexpected(vela::internal::CaptureHostError(execution, invoked.error()));
  }
  return *invoked;
}

}  // namespace

namespace aot {

Error TakeError(VelaAotStatus status, VelaAotError* error) {
  const std::unique_ptr<VelaAotError> owned(error);
  return owned ? std::move(owned->detail) : Error::FromStatus(static_cast<ExecutionStatus>(status));
}

VelaAotLocal Borrow(Local<vela::Value> value) noexcept {
  return {
      .scope = &vela::internal::HandleAccess::Scope(value),
      .slot = &vela::internal::HandleAccess::Slot(value),
  };
}

Result<VelaAotValue> Encode(HandleScope& scope, Value value) {
  VelaAotValue result{};
  if (value.IsReference()) {
    auto local = vela::internal::HandleAccess::Root<vela::Value>(scope, value);
    if (!local) {
      return std::unexpected(std::move(local.error()));
    }
    result.kind = VELA_AOT_LOCAL;
    result.local = Borrow(*local);
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

Result<Value> Decode(HandleScope& scope, const VelaAotValue& input) {
  if (input.reserved_byte != 0 || input.reserved_half != 0 || input.reserved_word != 0) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::TypeError));
  }
  switch (input.kind) {
    case VELA_AOT_NULL:
      if (input.payload == 0) {
        return Value::Null();
      }
      break;
    case VELA_AOT_BOOLEAN:
      if (input.payload <= 1) {
        return Value::Boolean(input.payload != 0);
      }
      break;
    case VELA_AOT_INTEGER:
      return Value::Integer(std::bit_cast<std::int64_t>(input.payload));
    case VELA_AOT_DOUBLE:
      return Value::Double(std::bit_cast<double>(input.payload));
    case VELA_AOT_LOCAL:
      if (input.local.scope == nullptr || &input.local.scope->isolate() != &scope.isolate()) {
        return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
      }
      return ReadSlot(input.local);
    default:
      break;
  }
  return std::unexpected(Error::FromStatus(ExecutionStatus::TypeError));
}

Result<Local<vela::Value>> ReceiveLocal(HandleScope& scope, const VelaAotValue& value) {
  const auto decoded = Decode(scope, value);
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  if (value.kind == VELA_AOT_LOCAL) {
    auto& slot = *static_cast<Value*>(value.local.slot);
    return vela::internal::HandleAccess::Borrow<vela::Value>(scope, slot);
  }
  return vela::internal::HandleAccess::Root<vela::Value>(scope, *decoded);
}

}  // namespace aot

extern "C" VelaAotIsolate* vela_aot_v2_isolate_create() {
  std::unique_ptr<Isolate> owned(new (std::nothrow) Isolate);
  return owned.release();
}

extern "C" void vela_aot_v2_isolate_release(VelaAotIsolate* isolate) {
  const std::unique_ptr<Isolate> owned(isolate);
}

extern "C" VelaAotStatus vela_aot_v2_scope_create(VelaAotIsolate* isolate, VelaAotScope** scope) {
  if (isolate == nullptr || !isolate->scheduler().IsOwnerThread()) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (scope == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  *scope = nullptr;
  auto* memory = isolate->heap().AllocateNative(sizeof(HandleScope));
  if (memory == nullptr) {
    return ToNativeStatus(ExecutionStatus::OutOfMemory);
  }
  *scope = std::construct_at(static_cast<HandleScope*>(memory), *isolate);
  return VELA_AOT_RETURNED;
}

extern "C" void vela_aot_v2_scope_release(VelaAotScope* scope) {
  if (scope != nullptr) {
    auto& heap = scope->isolate().heap();
    scope->~HandleScope();
    heap.FreeNative(scope);
  }
}

extern "C" VelaAotStatus vela_aot_v2_persist(VelaAotLocal local, VelaAotPersistent** persistent) {
  if (persistent == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  *persistent = nullptr;
  const auto value = ReadLocal(local);
  if (!value) {
    return ToNativeStatus(value.error().status);
  }
  auto& isolate = local.scope->isolate();
  auto root = HostRoot::Create(isolate, *value);
  if (!root) {
    return ToNativeStatus(root.error());
  }
  auto* memory = isolate.heap().AllocateNative(sizeof(VelaAotPersistent));
  if (memory == nullptr) {
    return ToNativeStatus(ExecutionStatus::OutOfMemory);
  }
  *persistent = std::construct_at(static_cast<VelaAotPersistent*>(memory), std::move(*root));
  return VELA_AOT_RETURNED;
}

extern "C" VelaAotStatus vela_aot_v2_localize(VelaAotScope* scope,
                                              const VelaAotPersistent* persistent,
                                              VelaAotLocal* local) {
  if (!IsOwner(scope)) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (persistent == nullptr || local == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  const auto& root = persistent->root;
  if (&scope->isolate() != &root.isolate()) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  const auto result = vela::internal::HandleAccess::Root<vela::Value>(*scope, root.value());
  if (!result) {
    return ToNativeStatus(result.error().status);
  }
  *local = aot::Borrow(*result);
  return VELA_AOT_RETURNED;
}

extern "C" void vela_aot_v2_persistent_release(VelaAotPersistent* persistent) {
  if (persistent != nullptr) {
    auto& heap = persistent->root.isolate().heap();
    persistent->~VelaAotPersistent();
    heap.FreeNative(persistent);
  }
}

extern "C" VelaAotStatus vela_aot_v2_instantiate(VelaAotIsolate* isolate,
                                                 const void* image,
                                                 VelaAotInstance** instance,
                                                 VelaAotError** error) {
  if (isolate == nullptr || !isolate->scheduler().IsOwnerThread()) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (instance == nullptr || image == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  *instance = nullptr;
  if (error != nullptr) {
    *error = nullptr;
  }
  auto created = NativeInstance::Instantiate(*isolate, *static_cast<const NativeImage*>(image));
  if (!created) {
    auto& failure = created.error();
    return StoreError(
        {
            .status = failure.status == NativeInstantiationStatus::OutOfMemory
                          ? ExecutionStatus::OutOfMemory
                          : ExecutionStatus::InvalidEntry,
            .message = std::move(failure.message),
            .stack_trace = {},
        },
        error);
  }
  *instance = created->release();
  return VELA_AOT_RETURNED;
}

extern "C" void vela_aot_v2_instance_release(VelaAotInstance* instance) {
  const std::unique_ptr<NativeInstance> owned(instance);
}

extern "C" VelaAotStatus vela_aot_v2_invoke(VelaAotScope* scope,
                                            VelaAotInstance* instance,
                                            const void* image,
                                            std::uint32_t function,
                                            const VelaAotValue* arguments,
                                            std::uint64_t count,
                                            VelaAotValue* result,
                                            VelaAotError** error) {
  if (!IsOwner(scope) || instance == nullptr || &scope->isolate() != &instance->isolate()) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (image != &instance->image() || result == nullptr || (count != 0 && arguments == nullptr)) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  if (error != nullptr) {
    *error = nullptr;
  }
  auto* parent = scope->isolate().native_control().state().host_execution;
  if (!CanReenterNativeProgram(parent, instance->code_identity())) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  NativeExecution execution(*instance, parent);
  auto invoked = Invoke(execution, *scope, function, {arguments, count});
  if (!invoked) {
    return StoreError(std::move(invoked.error()), error);
  }
  return StorePublic(*scope, *invoked, *result);
}

extern "C" VelaAotStatus vela_aot_v2_string_create(VelaAotScope* scope,
                                                   const char* text,
                                                   std::uint64_t size,
                                                   VelaAotLocal* result) {
  if (!IsOwner(scope)) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (result == nullptr || (text == nullptr && size != 0)) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  const auto string =
      vela::CreateString(*scope, std::string_view(text != nullptr ? text : "", size));
  if (!string) {
    return ToNativeStatus(string.error().status);
  }
  *result = aot::Borrow(string->ToValue());
  return VELA_AOT_RETURNED;
}

extern "C" VelaAotStatus vela_aot_v2_string_view(VelaAotLocal string,
                                                 const char** text,
                                                 std::uint64_t* size) {
  const auto value = ReadLocal(string);
  if (!value) {
    return ToNativeStatus(value.error().status);
  }
  if (text == nullptr || size == nullptr || !value->IsString()) {
    return ToNativeStatus(ExecutionStatus::TypeError);
  }
  const auto bytes = vela::internal::HostStringUtf8(*value);
  *text = bytes.data();
  *size = bytes.size();
  return VELA_AOT_RETURNED;
}

extern "C" VelaAotStatus vela_aot_v2_future_wait(VelaAotScope* scope,
                                                 VelaAotLocal future,
                                                 std::uint8_t wait_for_host,
                                                 VelaAotValue* result,
                                                 VelaAotError** error) {
  if (!IsOwner(scope) || future.scope == nullptr || &scope->isolate() != &future.scope->isolate()) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (result == nullptr || wait_for_host > 1) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  if (error != nullptr) {
    *error = nullptr;
  }
  const auto value = ReadSlot(future);
  if (!value) {
    return ToNativeStatus(value.error().status);
  }
  if (!value->IsObject() || Heap::GetFuture(*value->AsObject()) == nullptr) {
    return ToNativeStatus(ExecutionStatus::TypeError);
  }
  auto output = vela::internal::ReceiveHostValue<vela::Value>(*scope, [&](Value& destination) {
    return vela::internal::WaitHostFuture(
        *scope, *value, wait_for_host == 0 ? WaitMode::AvailableWork : WaitMode::WaitForHost,
        destination);
  });
  if (!output) {
    return StoreError(std::move(output.error()), error);
  }
  return StorePublic(*scope, vela::internal::HandleAccess::Read(*output), *result);
}

extern "C" const char* vela_aot_v2_status_code(VelaAotStatus status) {
  return status <= ToNativeStatus(ExecutionStatus::ExecutionTimedOut)
             ? ExecutionStatusCode(static_cast<ExecutionStatus>(status)).data()
             : "InvalidStatus";
}

extern "C" void vela_aot_v2_error_release(VelaAotError* error) {
  const std::unique_ptr<VelaAotError> owned(error);
}

extern "C" VelaAotStatus vela_aot_v2_error_payload(VelaAotScope* scope,
                                                   const VelaAotError* error,
                                                   VelaAotLocal* payload) {
  if (!IsOwner(scope)) {
    return ToNativeStatus(ExecutionStatus::InvalidIsolate);
  }
  if (error == nullptr || payload == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  const auto local = error->detail.payload.ToLocal(*scope);
  if (!local) {
    return ToNativeStatus(local.error().status);
  }
  *payload = aot::Borrow(*local);
  return VELA_AOT_RETURNED;
}

extern "C" VelaAotStatus vela_aot_v2_error_message(const VelaAotError* error,
                                                   const char** text,
                                                   std::uint64_t* size) {
  if (error == nullptr || text == nullptr || size == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  const auto& message = error->detail.message;
  *text = message.data();
  *size = message.size();
  return VELA_AOT_RETURNED;
}

extern "C" VelaAotStatus vela_aot_v2_error_frame(const VelaAotError* error,
                                                 std::uint64_t index,
                                                 VelaAotTraceFrame* frame) {
  if (error == nullptr || frame == nullptr) {
    return ToNativeStatus(ExecutionStatus::InvalidEntry);
  }
  const auto& trace = error->detail.stack_trace;
  if (index >= trace.size()) {
    return ToNativeStatus(ExecutionStatus::IndexOutOfBounds);
  }
  const auto& source = trace[index];
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
