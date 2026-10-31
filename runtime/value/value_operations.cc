#include "runtime/value/value_operations.h"

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

std::expected<std::uint32_t, ExecutionStatus> ArrayIndex(HeapObject* object, std::int64_t index) {
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (index < 0 || std::cmp_greater(index, std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  return static_cast<std::uint32_t>(index);
}

ExecutionStatus IndexError(ArrayAccessError error) {
  return error == ArrayAccessError::OutOfBounds ? ExecutionStatus::IndexOutOfBounds
                                                : ExecutionStatus::TypeError;
}

}  // namespace

bool IsEqualValue(Value left, Value right) noexcept {
  if (left.kind() != right.kind()) {
    return false;
  }
  switch (left.kind()) {
    case ValueKind::Null:
      return true;
    case ValueKind::Boolean:
      return left.AsBoolean() == right.AsBoolean();
    case ValueKind::Integer:
      return left.AsInteger() == right.AsInteger();
    case ValueKind::Double:
      return left.AsDouble() == right.AsDouble();
    case ValueKind::String:
      return left.AsString()->value() == right.AsString()->value();
    case ValueKind::Object: {
      const auto* layout = Heap::ClassOf(*left.AsObject());
      if (layout != nullptr && layout->IsEnum()) {
        const auto* right_layout = Heap::ClassOf(*right.AsObject());
        return right_layout != nullptr &&
               &layout->nominal_type() == &right_layout->nominal_type() &&
               Heap::GetField(*left.AsObject(), 0)->AsInteger() ==
                   Heap::GetField(*right.AsObject(), 0)->AsInteger();
      }
      return left.AsObject() == right.AsObject();
    }
  }
  std::unreachable();
}

std::expected<Value, ExecutionStatus> ConcatenateValues(Heap& heap, Value left, Value right) {
  if (left.IsString() && right.IsString()) {
    auto* string = heap.AllocateString(left.AsString()->value(), right.AsString()->value());
    if (string == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    return Value::String(string);
  }
  const std::array parts{left, right};
  const auto string = heap.JoinValues(parts);
  if (!string) {
    return std::unexpected(string.error() == StringJoinError::TypeMismatch
                               ? ExecutionStatus::TypeError
                               : ExecutionStatus::OutOfMemory);
  }
  return Value::String(*string);
}

std::expected<bool, ExecutionStatus> CompareStrings(Value left, Value right) {
  if (!left.IsString() || !right.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return left.AsString()->value() == right.AsString()->value();
}

std::expected<Value, ExecutionStatus> ReadIndex(HeapObject* object, std::int64_t index) {
  const auto offset = ArrayIndex(object, index);
  if (!offset) {
    return std::unexpected(offset.error());
  }
  const auto value = Heap::GetElement(*object, *offset);
  if (!value) {
    return std::unexpected(IndexError(value.error()));
  }
  return *value;
}

std::expected<void, ExecutionStatus> WriteIndex(Heap& heap,
                                                HeapObject* object,
                                                std::int64_t index,
                                                Value value) {
  const auto offset = ArrayIndex(object, index);
  if (!offset) {
    return std::unexpected(offset.error());
  }
  const auto written = heap.SetElement(*object, *offset, value);
  if (!written) {
    return std::unexpected(IndexError(written.error()));
  }
  return {};
}

std::expected<Value, ExecutionStatus> BindValueType(Value value,
                                                    const TypeConstraint& type) noexcept {
  if (value.IsNull() && type.nullable) {
    return value;
  }
  if (!value.IsObject()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto bound = Heap::BindRuntimeType(*value.AsObject(), type);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  return value;
}

std::expected<Value, ExecutionStatus> CheckFuturePayload(Value value,
                                                         const TypeConstraint& type) noexcept {
  if (type.kind != ValueKind::Object || type.object_kind != ObjectKind::Future ||
      type.signature == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto payload = type.signature->result;
  if (!Heap::IsValueOfType(value, type) &&
      !(payload ? Heap::IsValueOfType(value, *payload) : value.IsNull())) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  }
  return value;
}

std::expected<Value, ExecutionStatus> BoxDynamicValue(Heap& heap,
                                                      Value value,
                                                      const TypeConstraint& type) {
  const auto* instance = type.nominal;
  if (value.IsNull() || instance == nullptr || !instance->IsEnum()) {
    return value;
  }
  auto* object = heap.AllocateInstance(*instance->fixed_layout(), std::span(&value, 1));
  return object != nullptr ? std::expected<Value, ExecutionStatus>(Value::Object(object))
                           : std::unexpected(ExecutionStatus::OutOfMemory);
}

Value UnboxDynamicValue(Value value, const TypeConstraint& type) noexcept {
  return !value.IsNull() && type.nominal != nullptr && type.nominal->IsEnum()
             ? *Heap::GetField(*value.AsObject(), 0)
             : value;
}

}  // namespace vela::runtime
