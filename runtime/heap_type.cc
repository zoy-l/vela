#include "runtime/heap.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "base/execution_status.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

ClassLayout::ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields)
    : field_count_(field_count), reference_fields_(std::move(reference_fields)) {}

bool ClassLayout::IsImplementing(const InterfaceType& interface) const noexcept {
  return std::ranges::binary_search(interfaces_, &interface, std::less{});
}

void ClassLayout::BindCallableType(const TypeSignature& type) noexcept {
  assert(callable_.has_value());
  assert(callable_type_ == nullptr || callable_type_ == &type);
  callable_type_ = &type;
}

void ClassLayout::BindExpectedType(const TypeSignature& type) noexcept {
  assert(field_count_ == 1 && !callable_ && !is_enum_);
  assert(expected_type_ == nullptr || expected_type_ == &type);
  expected_type_ = &type;
}

void ClassLayout::BindCodeIdentity(const CodeIdentity& identity) noexcept {
  assert(callable_.has_value());
  assert(code_identity_ == nullptr || code_identity_ == &identity);
  code_identity_ = &identity;
}

std::optional<TypeConstraint> TypeSignature::FlattenedResult() const noexcept {
  if (!result || (*result).kind != ValueKind::Object ||
      (*result).object_kind != ObjectKind::Future) {
    return result;
  }
  auto payload = (*result).signature->result;
  if (payload && (*result).nullable && !(*payload).dynamic) {
    (*payload).nullable = true;
  }
  return payload;
}

const ClassLayout* Heap::ClassOf(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::Instance ? object.layout : nullptr;
}

TypeConstraint Heap::TypeOf(Value value) noexcept {
  TypeConstraint type{.kind = value.kind()};
  if (value.IsNull()) {
    type.nullable = true;
  } else if (value.IsObject()) {
    const auto& object = *value.AsObject();
    type.object_kind = KindOf(object);
    if (object.kind == ObjectKind::Future || object.kind == ObjectKind::Completer) {
      type.signature = object.future_signature();
      return type;
    }
    type.instance = ClassOf(object);
    type.array = ArrayTypeOf(object);
    type.collection = CollectionTypeOf(object);
    if (object.kind == ObjectKind::Expected) {
      type.signature = object.layout->expected_type();
    }
    if (type.instance != nullptr && type.instance->callable()) {
      type.callable = true;
      type.signature = type.instance->callable_type();
      type.instance = nullptr;
    }
  }
  return type;
}

bool TypeConstraint::IsSubtypeOf(const TypeConstraint& target) const noexcept {
  auto actual = *this;
  auto expected = target;
  while (true) {
    if (expected.dynamic) {
      return true;
    }
    if (actual.dynamic || (actual.nullable && !expected.nullable)) {
      return false;
    }
    actual.nullable = false;
    expected.nullable = false;
    if (actual == expected) {
      return true;
    }
    if ((expected.interface != nullptr) && actual.instance != nullptr &&
        actual.instance->IsImplementing(*expected.interface)) {
      return true;
    }
    if (actual.kind != ValueKind::Object || expected.kind != ValueKind::Object ||
        actual.object_kind != ObjectKind::Future || expected.object_kind != ObjectKind::Future ||
        actual.signature == nullptr || expected.signature == nullptr) {
      return false;
    }
    const auto& actual_result = actual.signature->result;
    const auto& expected_result = expected.signature->result;
    if (!actual_result || !expected_result) {
      return !actual_result && !expected_result;
    }
    actual = *actual_result;
    expected = *expected_result;
  }
}

bool Heap::IsThrowable(Value value) noexcept {
  const auto* layout = value.IsObject() ? ClassOf(*value.AsObject()) : nullptr;
  return layout != nullptr && !layout->IsEnum() && !layout->callable();
}

bool Heap::IsValueOfType(Value value, const TypeConstraint& type) noexcept {
  if (type.dynamic) {
    return true;
  }
  if (value.IsNull()) {
    return type.nullable;
  }
  if (value.kind() != type.kind) {
    return false;
  }
  if (!value.IsObject()) {
    return true;
  }
  const auto* object = value.AsObject();
  if (object == nullptr || object->kind != type.object_kind ||
      (type.instance != nullptr && object->layout != type.instance) ||
      (type.array != nullptr && object->array_layout() != type.array) ||
      (type.collection != nullptr && object->collection_layout() != type.collection)) {
    return false;
  }
  if ((type.interface != nullptr) &&
      (object->layout == nullptr || !object->layout->IsImplementing(*type.interface))) {
    return false;
  }
  if ((type.signature != nullptr) && !type.callable && !TypeOf(value).IsSubtypeOf(type)) {
    return false;
  }
  return !type.callable ||
         (object->layout != nullptr && object->layout->callable().has_value() &&
          (type.signature == nullptr || object->layout->callable_type() == type.signature));
}

std::expected<void, ExecutionStatus> Heap::BindRuntimeType(HeapObject& object,
                                                           const TypeConstraint& type) noexcept {
  if ((object.kind != ObjectKind::Future && object.kind != ObjectKind::Completer) ||
      type.kind != ValueKind::Object || object.kind != type.object_kind ||
      type.signature == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (object.future_signature() != nullptr) {
    return TypeOf(Value::Object(&object)).IsSubtypeOf(type)
               ? std::expected<void, ExecutionStatus>{}
               : std::unexpected(ExecutionStatus::InvalidCast);
  }
  object.type_metadata = type.signature;
  return {};
}

}  // namespace vela::runtime
