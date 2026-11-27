#include "runtime/heap/heap.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "base/execution_status.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

ClassLayout::ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields)
    : field_count_(field_count), reference_fields_(std::move(reference_fields)) {}

NominalType::NominalType(NominalTypeOptions options)
    : kind_(options.kind),
      interfaces_(std::move(options.interfaces)),
      declaration_(options.declaration) {
  std::ranges::sort(interfaces_, std::less{});
}

bool NominalType::IsImplementing(const InterfaceType& interface) const noexcept {
  return std::ranges::binary_search(interfaces_, &interface, std::less{});
}

bool NominalType::IsSubtypeOf(const NominalType& target) const noexcept {
  for (const auto* current = this; current != nullptr; current = current->base_) {
    if (current == &target) {
      return true;
    }
  }
  return false;
}

void NominalType::BindBase(const NominalType& base) noexcept {
  assert(base_ == nullptr || base_ == &base);
  base_ = &base;
}

bool ClassLayout::IsImplementing(const InterfaceType& interface) const noexcept {
  return nominal_->IsImplementing(interface);
}

void ClassLayout::BindCallableType(const TypeSignature& type) noexcept {
  assert(callable_.has_value());
  assert(callable_type_ == nullptr || callable_type_ == &type);
  callable_type_ = &type;
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
  if (payload && (*result).nullable && !(*payload).IsDynamic()) {
    if (payload->IsNever()) {
      payload = TypeConstraint{.nullable = true};
    } else {
      payload->nullable = true;
    }
  }
  return payload;
}

bool TypeSignature::IsSubtypeOf(const TypeSignature& target) const noexcept {
  if (this == &target) {
    return true;
  }
  if (parameters != target.parameters ||
      (!(result && result->IsNever()) && result != target.result)) {
    return false;
  }
  const auto matched =
      !target.narrowed_parameter ||
      (narrowed_parameter && narrowed_parameter->IsSubtypeOf(*target.narrowed_parameter));
  const auto unmatched = !target.false_narrowed_parameter ||
                         (false_narrowed_parameter &&
                          false_narrowed_parameter->IsSubtypeOf(*target.false_narrowed_parameter));
  return matched && unmatched;
}

const ClassLayout* Heap::ClassOf(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::Instance || object.kind == ObjectKind::Host ? object.layout
                                                                                : nullptr;
}

TypeConstraint Heap::TypeOf(Value value) noexcept {
  TypeConstraint type{.kind = value.kind()};
  if (value.IsNull()) {
    type.nullable = true;
  } else if (value.IsObject()) {
    const auto& object = *value.AsObject();
    type.object_kind = KindOf(object);
    if (HasFuturePayload(object.kind)) {
      type.signature = object.future_signature();
      return type;
    }
    const auto* layout = ClassOf(object);
    type.nominal = layout == nullptr ? nullptr : &layout->nominal_type();
    type.array = ArrayTypeOf(object);
    type.collection = CollectionTypeOf(object);
    if (layout != nullptr && (layout->callable() != nullptr)) {
      type.callable = true;
      type.signature = layout->callable_type();
      type.nominal = nullptr;
    }
  }
  return type;
}

namespace {

bool IsNominalSubtype(const TypeConstraint& actual, const TypeConstraint& expected) noexcept {
  if (actual.nominal == nullptr) {
    return false;
  }
  if (expected.nominal != nullptr && actual.kind == expected.kind &&
      actual.object_kind == expected.object_kind &&
      actual.nominal->IsSubtypeOf(*expected.nominal)) {
    return true;
  }
  return expected.interface != nullptr && actual.nominal->IsImplementing(*expected.interface);
}

bool IsTypedFuture(const TypeConstraint& type) noexcept {
  return type.kind == ValueKind::Object && type.object_kind == ObjectKind::Future &&
         type.signature != nullptr;
}

// 无结果表示两侧都是 Future，需要继续比较载荷；其余类型在本层即可判定。
std::optional<bool> CompareOuterTypes(TypeConstraint actual, TypeConstraint expected) noexcept {
  if (actual.IsNever()) {
    return true;
  }
  if (expected.IsNever()) {
    return false;
  }
  if (expected.IsDynamic()) {
    return true;
  }
  if (!actual.IsDynamic() && actual.kind == ValueKind::Null) {
    return expected.nullable || expected.kind == ValueKind::Null;
  }
  if (actual.IsDynamic() || (actual.nullable && !expected.nullable)) {
    return false;
  }
  actual.nullable = false;
  expected.nullable = false;
  if (actual == expected) {
    return true;
  }
  if (actual.callable && expected.callable && actual.signature != nullptr &&
      expected.signature != nullptr) {
    return actual.signature->IsSubtypeOf(*expected.signature);
  }
  if (IsNominalSubtype(actual, expected)) {
    return true;
  }
  if (!IsTypedFuture(actual) || !IsTypedFuture(expected)) {
    return false;
  }
  return std::nullopt;
}

}  // namespace

bool TypeConstraint::IsSubtypeOf(const TypeConstraint& target) const noexcept {
  auto actual = *this;
  auto expected = target;
  while (true) {
    if (const auto result = CompareOuterTypes(actual, expected)) {
      return *result;
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
  return layout != nullptr && !layout->IsEnum() && (layout->callable() == nullptr);
}

bool Heap::IsValueOfType(Value value, const TypeConstraint& type) noexcept {
  if (type.IsNever()) {
    return false;
  }
  if (type.IsDynamic()) {
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
  if (object == nullptr) {
    return false;
  }
  const auto* layout = object->layout;
  if (object->kind != type.object_kind ||
      (type.nominal != nullptr &&
       (layout == nullptr || !layout->nominal_type().IsSubtypeOf(*type.nominal))) ||
      (type.array != nullptr && object->array_layout() != type.array) ||
      (type.collection != nullptr && object->collection_layout() != type.collection)) {
    return false;
  }
  if ((type.interface != nullptr) &&
      (layout == nullptr || !layout->IsImplementing(*type.interface))) {
    return false;
  }
  if ((type.signature != nullptr) && !type.callable && !TypeOf(value).IsSubtypeOf(type)) {
    return false;
  }
  if (!type.callable) {
    return true;
  }
  if (layout == nullptr || layout->callable() == nullptr) {
    return false;
  }
  const auto* callable_type = layout->callable_type();
  return type.signature == nullptr ||
         (callable_type != nullptr && callable_type->IsSubtypeOf(*type.signature));
}

std::expected<void, ExecutionStatus> Heap::BindRuntimeType(HeapObject& object,
                                                           const TypeConstraint& type) noexcept {
  if (!HasFuturePayload(object.kind) || type.kind != ValueKind::Object ||
      object.kind != type.object_kind || type.signature == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (object.future_signature() != nullptr) {
    return TypeOf(Value::Object(&object)).IsSubtypeOf(type)
               ? std::expected<void, ExecutionStatus>{}
               : std::unexpected(ExecutionStatus::InvalidCast);
  }
  object.SetTypeMetadata(type.signature);
  return {};
}

}  // namespace vela::runtime
