#include "runtime/native_operations.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/execution_status.h"
#include "base/regexp.h"
#include "runtime/builtins.h"
#include "runtime/byte_operations.h"
#include "runtime/date.h"
#include "runtime/dynamic_value.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_async.h"
#include "runtime/native_dynamic.h"
#include "runtime/native_instance.h"
#include "runtime/native_json.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/value.h"
#include "runtime/value_buffer.h"
#include "runtime/value_io.h"
#include "runtime/value_operations.h"

namespace vela::runtime {
namespace {

std::uint8_t Status(ExecutionStatus status) noexcept {
  return static_cast<std::uint8_t>(status);
}

std::uint8_t Store(NativeValue* destination, std::expected<Value, ExecutionStatus> result) {
  if (!result) {
    return Status(result.error());
  }
  *destination = EncodeNativeValue(*result);
  return Status(ExecutionStatus::Returned);
}

std::uint8_t StoreObject(NativeValue* destination, HeapObject* object) noexcept {
  if (object == nullptr) {
    return Status(ExecutionStatus::OutOfMemory);
  }
  *destination = EncodeNativeValue(Value::Object(object));
  return Status(ExecutionStatus::Returned);
}

}  // namespace

extern "C" void vela_native_v1_constant(NativeExecution* execution,
                                        std::uint32_t index,
                                        NativeValue* result) noexcept {
  *result = EncodeNativeValue(execution->instance().constant(index));
}

extern "C" std::uint8_t vela_native_v1_type_test(NativeExecution* execution,
                                                 std::uint32_t type,
                                                 const NativeValue* value) noexcept {
  return Heap::IsValueOfType(DecodeNativeValue(*value),
                             execution->instance().layouts().types.at(type))
             ? 1
             : 0;
}

extern "C" std::uint8_t vela_native_v2_initialize_global(NativeExecution* execution,
                                                         std::uint32_t index,
                                                         std::uint64_t position,
                                                         NativeGlobalFrame frame_kind,
                                                         NativeValue* result) {
  NativeTraceFrame frame;
  if (frame_kind == NativeGlobalFrame::Leaf) {
    const auto decoded = DecodeNativeTracePosition(position);
    execution->MaterializeFrame(frame, decoded.function);
    *execution->current_instruction() = decoded.instruction;
  }
  const auto status = Store(result, execution->instance().InitializeGlobal(*execution, index));
  if (frame_kind == NativeGlobalFrame::Leaf) {
    execution->Exit();
  }
  return status;
}

extern "C" std::uint8_t vela_native_v1_new_expected(NativeExecution* execution,
                                                    std::uint32_t layout,
                                                    const NativeValue* payload,
                                                    std::uint8_t alternative,
                                                    NativeValue* result) noexcept {
  auto* type = execution->instance().layouts().classes.at(layout);
  return StoreObject(result, execution->isolate().heap().AllocateExpected(
                                 *type, DecodeNativeValue(*payload),
                                 static_cast<ExpectedAlternative>(alternative)));
}

extern "C" std::uint8_t vela_native_v1_expected_has_value(HeapObject* object,
                                                          NativeValue* result) noexcept {
  return Store(result, ExpectedHasValue(object));
}

extern "C" std::uint8_t vela_native_v1_expected_value(HeapObject* object,
                                                      std::uint8_t alternative,
                                                      NativeValue* result) noexcept {
  return Store(result, ExpectedValue(object, static_cast<ExpectedAlternative>(alternative)));
}

extern "C" std::uint8_t vela_native_v1_new_instance(NativeExecution* execution,
                                                    std::uint32_t index,
                                                    NativeValue* result) noexcept {
  const auto* type = execution->instance().layouts().classes.at(index);
  auto& heap = execution->isolate().heap();
  auto* object = heap.AllocateInstance(*type);
  if (object != nullptr && type->callable()) {
    heap.SetNativeEnvironment(*object, *execution->instance().state().front().AsObject());
  }
  return StoreObject(result, object);
}

extern "C" std::uint8_t vela_native_v1_new_array(NativeExecution* execution,
                                                 std::uint32_t index,
                                                 NativeValue* result,
                                                 std::int64_t length) noexcept {
  if (length < 0) {
    return Status(ExecutionStatus::IndexOutOfBounds);
  }
  const auto* type = execution->instance().layouts().arrays.at(index);
  return StoreObject(
      result, execution->isolate().heap().AllocateArray(*type, static_cast<std::size_t>(length)));
}

extern "C" std::uint8_t vela_native_v1_new_collection(NativeExecution* execution,
                                                      std::uint32_t index,
                                                      NativeValue* result) noexcept {
  const auto* type = execution->instance().layouts().collections.at(index);
  return StoreObject(result, execution->isolate().heap().AllocateCollection(*type));
}

extern "C" void vela_native_v1_write_barrier(NativeExecution* execution,
                                             HeapObject* object,
                                             const NativeValue* value) noexcept {
  const auto decoded = DecodeNativeValue(*value);
  if (decoded.IsReference()) {
    execution->isolate().heap().WriteBarrier(*object, decoded);
  }
}

extern "C" std::uint8_t vela_native_v1_array_grow(NativeExecution* execution,
                                                  HeapObject* array,
                                                  const NativeValue* value) noexcept {
  const auto grown = execution->isolate().heap().GrowArray(*array, DecodeNativeValue(*value));
  return Status(grown ? ExecutionStatus::Returned : ExecutionStatus::OutOfMemory);
}

extern "C" std::uint8_t vela_native_v1_builtin(NativeExecution* execution,
                                               std::uint32_t operation,
                                               const NativeValue* arguments,
                                               NativeValue* result) {
  const auto builtin = static_cast<Builtin>(operation);
  const auto count = BuiltinArity(builtin);
  const auto input = std::span(arguments, count);
  std::array<Value, kMaximumBuiltinArity> values;
  const auto decoded = std::span(values).first(count);
  for (std::size_t index = 0; index < count; ++index) {
    decoded.subspan(index).front() = DecodeNativeValue(input.subspan(index).front());
  }
  auto& heap = execution->isolate().heap();
  if (builtin >= Builtin::DynamicBinary && builtin <= Builtin::DynamicIndexWrite) {
    return Store(result, ExecuteDynamicValue(builtin, decoded, heap));
  }
  if (builtin == Builtin::DynamicAsync) {
    return Store(result, ExecuteNativeDynamicAsync(*execution, decoded));
  }
  if (builtin >= Builtin::DynamicCall && builtin <= Builtin::DynamicArrayValidate) {
    return Store(result, ExecuteNativeDynamicCall(*execution, builtin, decoded));
  }
  if (builtin == Builtin::JsonParse || builtin == Builtin::JsonStringify) {
    return Store(result, ExecuteNativeJson(*execution, builtin, decoded.front()));
  }
  if (builtin >= Builtin::DateParse && builtin <= Builtin::DateToUTC) {
    const ClassLayout* layout = nullptr;
    if (builtin == Builtin::DateToUTC) {
      const auto index = static_cast<std::size_t>(decoded.subspan(1).front().AsInteger());
      layout = execution->instance().layouts().classes.at(index);
    }
    const auto value = ExecuteDateBuiltin(builtin, decoded, heap, layout);
    return value ? Store(result, *value)
                 : Status(execution->HandleFailure(value.error().status, value.error().message));
  }
  if (IsRegExpBuiltin(builtin)) {
    const ArrayLayout* layout = nullptr;
    if (HasRegExpArrayLayout(builtin)) {
      const auto index = static_cast<std::size_t>(decoded.back().AsInteger());
      layout = execution->instance().layouts().arrays.at(index);
    }
    const auto value = ExecuteRegExpBuiltin(builtin, decoded, heap, layout);
    return value ? Store(result, *value)
                 : Status(execution->HandleFailure(value.error().status, value.error().message));
  }
  if (builtin == Builtin::BindRuntimeType || builtin == Builtin::CheckFuturePayload) {
    const auto value = decoded.front();
    const auto type_index = static_cast<std::size_t>(decoded.subspan(1).front().AsInteger());
    const auto& type = execution->instance().layouts().types.at(type_index);
    return Store(result, builtin == Builtin::BindRuntimeType ? BindValueType(value, type)
                                                             : CheckFuturePayload(value, type));
  }
  return Store(result, IsByteBuiltin(builtin) ? ExecuteByteBuiltin(builtin, decoded, heap)
                                              : ExecuteBuiltin(builtin, decoded, heap));
}

extern "C" std::uint8_t vela_native_v1_concat(NativeExecution* execution,
                                              const NativeValue* left,
                                              const NativeValue* right,
                                              NativeValue* result) {
  return Store(result, ConcatenateValues(execution->isolate().heap(), DecodeNativeValue(*left),
                                         DecodeNativeValue(*right)));
}

extern "C" std::uint8_t vela_native_v1_interpolate(NativeExecution* execution,
                                                   const NativeValue* arguments,
                                                   std::uint64_t count,
                                                   NativeValue* result) {
  ValueBuffer storage(count);
  if (!storage.IsAllocated()) {
    return Status(ExecutionStatus::OutOfMemory);
  }
  const auto values = storage.values();
  const auto input = std::span(arguments, count);
  for (std::size_t index = 0; index < values.size(); ++index) {
    values.subspan(index).front() = DecodeNativeValue(input.subspan(index).front());
  }
  const auto string = execution->isolate().heap().JoinValues(values);
  if (!string) {
    return Status(string.error() == StringJoinError::TypeMismatch ? ExecutionStatus::TypeError
                                                                  : ExecutionStatus::OutOfMemory);
  }
  *result = EncodeNativeValue(Value::String(*string));
  return Status(ExecutionStatus::Returned);
}

extern "C" std::uint8_t vela_native_v1_compare_strings(const NativeValue* left,
                                                       const NativeValue* right,
                                                       NativeValue* result) {
  const auto compared = CompareStrings(DecodeNativeValue(*left), DecodeNativeValue(*right));
  if (!compared) {
    return Status(compared.error());
  }
  *result = EncodeNativeValue(Value::Boolean(*compared));
  return Status(ExecutionStatus::Returned);
}

extern "C" std::uint8_t vela_native_v1_equal(const NativeValue* left,
                                             const NativeValue* right) noexcept {
  return IsEqualValue(DecodeNativeValue(*left), DecodeNativeValue(*right)) ? 1 : 0;
}

extern "C" void vela_native_v1_print(NativeExecution* execution, const NativeValue* value) {
  auto& output = execution->isolate().output();
  WriteValue(output, DecodeNativeValue(*value));
  output << '\n';
}

}  // namespace vela::runtime
