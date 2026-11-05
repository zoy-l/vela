#include "runtime/native/handle_access.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"
#include "vela/error.h"

namespace vela::runtime {

NativeResult internal::BoxNativeArgument(NativeExecution& execution,
                                         std::uint32_t function,
                                         Value value,
                                         std::size_t argument) {
  const auto& instance = execution.instance();
  const auto& signature = instance.call_signature(function);
  const auto receiver_count = static_cast<std::size_t>(signature.receiver.has_value());
  const auto index = signature.receiver && argument == 0
                         ? *signature.receiver
                         : signature.parameters[argument - receiver_count].type;
  const auto boxed =
      BoxDynamicValue(execution.isolate().heap(), value, instance.layouts().types[index]);
  if (!boxed) {
    return std::unexpected(vela::Error{.status = boxed.error(), .message = {}, .stack_trace = {}});
  }
  return *boxed;
}

std::expected<Value, ExecutionStatus> internal::CompleteNativeResult(NativeExecution& execution,
                                                                     std::uint32_t function,
                                                                     NativeResult result) {
  result = ValidateNativeResult(std::move(result));
  if (!result) {
    auto& error = result.error();
    const auto status = error.status;
    if (status == ExecutionStatus::OutOfMemory) {
      execution.AbortWithOutOfMemory();
      return std::unexpected(status);
    }
    if (status == ExecutionStatus::ExecutionStopped) {
      return std::unexpected(status);
    }
    if (!error.payload.IsEmpty()) {
      const auto& payload = vela::internal::HandleAccess::ReadErrorPayload(error);
      if (&payload.isolate() != &execution.isolate()) {
        return std::unexpected(execution.HandleFailure(ExecutionStatus::InvalidIsolate));
      }
      return std::unexpected(execution.RestoreHostError(std::move(error)));
    }
    return std::unexpected(execution.HandleFailure(status, error.message));
  }
  const auto& instance = execution.instance();
  const auto result_type = instance.image().function(function).result_type;
  if (result_type == kNoNativeIndex) {
    return *result;
  }
  const auto& type = instance.layouts().types.at(result_type);
  if (!Heap::IsValueOfType(*result, type)) {
    return std::unexpected(execution.HandleFailure(
        ExecutionStatus::TypeError, "native result does not match its external declaration"));
  }
  return *result;
}

extern "C" const NativeFunction::AotEntry* vela_native_v1_import(NativeExecution* execution,
                                                                 std::uint32_t function) noexcept {
  return &execution->instance().import_entry(function);
}

}  // namespace vela::runtime
