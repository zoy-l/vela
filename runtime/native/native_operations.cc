#include "runtime/native/native_operations.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/execution_status.h"
#include "base/regexp.h"
#include "runtime/collections/byte_operations.h"
#include "runtime/dynamic/dynamic_value.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/library/builtins.h"
#include "runtime/library/date.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_async.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_dynamic.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_json.h"
#include "runtime/native/native_mock.h"
#include "runtime/native/native_test.h"
#include "runtime/regexp/regexp.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_buffer.h"
#include "runtime/value/value_io.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
using internal::StoreNativeResult;

namespace {

std::uint8_t StoreObject(NativeValue* destination, HeapObject* object) noexcept {
  if (object == nullptr) {
    return ToNativeStatus(ExecutionStatus::OutOfMemory);
  }
  *destination = EncodeNativeValue(Value::Object(object));
  return ToNativeStatus(ExecutionStatus::Returned);
}

template <class Failure>
std::expected<Value, ExecutionStatus> NativeLibraryResult(NativeExecution& execution,
                                                          std::expected<Value, Failure> result) {
  return std::move(result).transform_error([&execution](const Failure& error) {
    return execution.HandleFailure(error.status, error.message);
  });
}

std::expected<Value, ExecutionStatus> ExecuteNativeDate(NativeExecution& execution,
                                                        Builtin builtin,
                                                        std::span<const Value> arguments) {
  const ClassLayout* layout = nullptr;
  if (builtin == Builtin::DateToUTC) {
    const auto index = static_cast<std::size_t>(arguments.subspan(1).front().AsInteger());
    layout = execution.instance().layouts().classes.at(index);
  }
  return NativeLibraryResult(
      execution, ExecuteDateBuiltin(builtin, arguments, execution.isolate().heap(), layout));
}

std::expected<Value, ExecutionStatus> ExecuteNativeRegExp(NativeExecution& execution,
                                                          Builtin builtin,
                                                          std::span<const Value> arguments) {
  const ArrayLayout* layout = nullptr;
  if (HasRegExpArrayLayout(builtin)) {
    const auto index = static_cast<std::size_t>(arguments.back().AsInteger());
    layout = execution.instance().layouts().arrays.at(index);
  }
  return NativeLibraryResult(
      execution, ExecuteRegExpBuiltin(builtin, arguments, execution.isolate().heap(), layout));
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
  const auto status =
      StoreNativeResult(result, execution->instance().InitializeGlobal(*execution, index));
  if (frame_kind == NativeGlobalFrame::Leaf) {
    execution->Exit();
  }
  return status;
}

namespace {

const ClassLayout& AllocationLayout(NativeExecution& execution, std::uint32_t index) noexcept {
  const auto* type = execution.instance().layouts().classes.at(index);
  if (type->storage() == ClassStorage::Reloadable) {
    auto* dispatch = execution.instance().dispatch();
    type = &dispatch->CurrentLayout(type->nominal_type());
  }
  return *type;
}

}  // namespace

extern "C" HeapObject* vela_native_v10_new_object(NativeExecution* execution,
                                                  std::uint32_t index) noexcept {
  return execution->isolate().heap().AllocateInstance(AllocationLayout(*execution, index));
}

extern "C" std::uint8_t vela_native_v10_new_callable(NativeExecution* execution,
                                                     std::uint32_t index,
                                                     NativeValue* result) noexcept {
  const auto object = AllocateNativeCallable(*execution, AllocationLayout(*execution, index));
  return object ? StoreObject(result, *object) : ToNativeStatus(object.error());
}

extern "C" HeapObject* vela_native_v10_new_array(NativeExecution* execution,
                                                 std::uint32_t index,
                                                 std::uint64_t length) noexcept {
  const auto* type = execution->instance().layouts().arrays.at(index);
  return execution->isolate().heap().AllocateArray(*type, static_cast<std::size_t>(length));
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

extern "C" std::uint64_t vela_native_v9_scalar_text_length(const NativeValue* value) {
  return ScalarTextLength(DecodeNativeValue(*value));
}

extern "C" StringObject* vela_native_v10_scalar_to_string(NativeExecution* execution,
                                                          const NativeValue* value) noexcept {
  return execution->isolate().heap().AllocateScalarString(DecodeNativeValue(*value));
}

extern "C" void vela_native_v11_map_get(HeapObject* map,
                                        const NativeValue* key,
                                        NativeValue* result) noexcept {
  *result = EncodeNativeValue(Heap::LoadMapValue(*map, DecodeNativeValue(*key)));
}

extern "C" std::uint8_t vela_native_v11_map_set(NativeExecution* execution,
                                                HeapObject* map,
                                                const NativeValue* key,
                                                const NativeValue* value) noexcept {
  const auto inserted = execution->isolate().heap().StoreMapValue(*map, DecodeNativeValue(*key),
                                                                  DecodeNativeValue(*value));
  return ToNativeStatus(inserted);
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
  if (builtin == Builtin::TestRegister || builtin == Builtin::TestControl) {
    return StoreNativeResult(result, ExecuteNativeTest(*execution, builtin, decoded));
  }
  if (builtin == Builtin::MockInspect) {
    return StoreNativeResult(result, ExecuteNativeMock(*execution, decoded));
  }
  if (builtin >= Builtin::DynamicBinary && builtin <= Builtin::DynamicIndexWrite) {
    return StoreNativeResult(result, ExecuteDynamicValue(builtin, decoded, heap));
  }
  if (builtin == Builtin::DynamicAsync) {
    return StoreNativeResult(result, ExecuteNativeDynamicAsync(*execution, decoded));
  }
  if (builtin >= Builtin::DynamicCall && builtin <= Builtin::DynamicArrayValidate) {
    return StoreNativeResult(result, ExecuteNativeDynamicCall(*execution, builtin, decoded));
  }
  if (builtin == Builtin::JsonParse || builtin == Builtin::JsonStringify) {
    return StoreNativeResult(result, ExecuteNativeJson(*execution, builtin, decoded.front()));
  }
  if (builtin == Builtin::JsonTypedParse || builtin == Builtin::JsonSafeParse) {
    return StoreNativeResult(result, ExecuteNativeTypedJSON(*execution, builtin, decoded));
  }
  if (builtin >= Builtin::DateParse && builtin <= Builtin::DateToUTC) {
    return StoreNativeResult(result, ExecuteNativeDate(*execution, builtin, decoded));
  }
  if (IsRegExpBuiltin(builtin)) {
    return StoreNativeResult(result, ExecuteNativeRegExp(*execution, builtin, decoded));
  }
  if (builtin == Builtin::BindRuntimeType || builtin == Builtin::CheckFuturePayload) {
    const auto value = decoded.front();
    const auto type_index = static_cast<std::size_t>(decoded.subspan(1).front().AsInteger());
    const auto& type = execution->instance().layouts().types.at(type_index);
    return StoreNativeResult(result, builtin == Builtin::BindRuntimeType
                                         ? BindValueType(value, type)
                                         : CheckFuturePayload(value, type));
  }
  return StoreNativeResult(result, IsByteBuiltin(builtin)
                                       ? ExecuteByteBuiltin(builtin, decoded, heap)
                                       : ExecuteBuiltin(builtin, decoded, heap));
}

extern "C" std::uint8_t vela_native_v1_concat(NativeExecution* execution,
                                              const NativeValue* left,
                                              const NativeValue* right,
                                              NativeValue* result) {
  return StoreNativeResult(result,
                           ConcatenateValues(execution->isolate().heap(), DecodeNativeValue(*left),
                                             DecodeNativeValue(*right)));
}

extern "C" std::uint8_t vela_native_v1_interpolate(NativeExecution* execution,
                                                   const NativeValue* arguments,
                                                   std::uint64_t count,
                                                   NativeValue* result) {
  ValueBuffer storage(count);
  if (!storage.IsAllocated()) {
    return ToNativeStatus(ExecutionStatus::OutOfMemory);
  }
  const auto values = storage.values();
  const auto input = std::span(arguments, count);
  for (std::size_t index = 0; index < values.size(); ++index) {
    values.subspan(index).front() = DecodeNativeValue(input.subspan(index).front());
  }
  const auto string = execution->isolate().heap().JoinValues(values);
  if (!string) {
    return ToNativeStatus(StringJoinStatus(string.error()));
  }
  *result = EncodeNativeValue(Value::String(*string));
  return ToNativeStatus(ExecutionStatus::Returned);
}

extern "C" std::uint8_t vela_native_v2_compare_strings(const NativeValue* left,
                                                       const NativeValue* right) noexcept {
  const auto first = DecodeNativeValue(*left).AsString()->value();
  const auto second = DecodeNativeValue(*right).AsString()->value();
  return first == second ? 1 : 0;
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
