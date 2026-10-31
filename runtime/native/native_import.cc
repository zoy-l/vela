#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"

#include <cstdint>
#include <expected>
#include <utility>

#include "base/execution_status.h"
#include "runtime/async/future_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

std::expected<Value, ExecutionStatus> internal::CompleteNativeResult(NativeExecution& execution,
                                                                     std::uint32_t function,
                                                                     NativeResult result) {
  result = ValidateNativeResult(std::move(result));
  if (!result) {
    const auto& error = result.error();
    const auto status = error.kind == NativeErrorKind::OutOfMemory ? ExecutionStatus::OutOfMemory
                                                                   : ExecutionStatus::NativeFailure;
    return std::unexpected(execution.HandleFailure(status, error.message));
  }
  const auto& instance = execution.instance();
  const auto result_type =
      instance.image().functions.values().subspan(function).front().result_type;
  if (result_type == kNoNativeIndex) {
    return *result;
  }
  const auto& type = instance.layouts().types.at(result_type);
  if (type.object_kind != ObjectKind::Future) {
    return *result;
  }
  auto checked = CheckFutureCompletion(
      execution.isolate(), *result, type.signature->result,
      execution.Diagnostic(ExecutionStatus::TypeError,
                           "native Future completion does not match its external declaration"),
      instance.state());
  if (!checked) {
    return std::unexpected(checked.error());
  }
  const auto bound = Heap::BindRuntimeType(*checked->AsObject(), type);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  return *checked;
}

extern "C" const NativeFunction::AotEntry* vela_native_v1_import(NativeExecution* execution,
                                                                 std::uint32_t function) noexcept {
  return &execution->instance().import_entry(function);
}

}  // namespace vela::runtime
