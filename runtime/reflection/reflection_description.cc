#include "runtime/reflection/reflection_description.h"

#include <array>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "base/reflection.h"
#include "runtime/native/native_image.h"
#include "runtime/reflection/reflection_index.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

namespace {

reflection::TypeKind ReflectedKind(ReflectedType type) noexcept {
  using reflection::TypeKind;
  const auto& constraint = type.constraint;
  if (type.is_void) {
    return TypeKind::Void;
  }
  if (constraint.dynamic) {
    return TypeKind::Dynamic;
  }
  switch (constraint.kind) {
    case ValueKind::Null:
      return TypeKind::Null;
    case ValueKind::Boolean:
      return TypeKind::Bool;
    case ValueKind::Integer:
      return TypeKind::Int;
    case ValueKind::Double:
      return TypeKind::Double;
    case ValueKind::String:
      return TypeKind::String;
    case ValueKind::Object:
      break;
  }
  if (constraint.callable) {
    return TypeKind::Function;
  }
  if (constraint.interface != nullptr) {
    return TypeKind::Interface;
  }
  if (constraint.nominal != nullptr && constraint.nominal->IsEnum()) {
    return TypeKind::Enum;
  }
  switch (constraint.object_kind) {
    case ObjectKind::Array:
      return TypeKind::Array;
    case ObjectKind::Collection:
      return constraint.collection->values ? TypeKind::Map : TypeKind::Set;
    case ObjectKind::MutableString:
      return TypeKind::MutableString;
    case ObjectKind::Future:
      return TypeKind::Future;
    case ObjectKind::Completer:
      return TypeKind::Completer;
    case ObjectKind::Timer:
      return TypeKind::Timer;
    case ObjectKind::CancellationToken:
      return TypeKind::CancellationToken;
    case ObjectKind::ByteArray:
      return TypeKind::ByteArray;
    case ObjectKind::ByteData:
      return TypeKind::ByteData;
    case ObjectKind::RegExp:
      return TypeKind::RegExp;
    case ObjectKind::RegExpMatch:
      return TypeKind::RegExpMatch;
    default:
      return TypeKind::Class;
  }
}

void DescribeSignature(ReflectedTypeDescription& description, const TypeConstraint& type) {
  const auto& signature = *type.signature;
  const auto returned = signature.result ? ReflectedType{.constraint = *signature.result}
                                         : ReflectedType{.constraint = {}, .is_void = true};
  if (type.callable) {
    description.signature = ReflectedSignature{.parameters = {}, .return_type = returned};
    for (const auto& parameter : signature.parameters) {
      description.signature->parameters.push_back({
          .type = {.constraint = parameter.type},
          .is_optional = parameter.optional,
          .is_rest = parameter.rest,
      });
    }
  } else if (HasFuturePayload(type.object_kind)) {
    description.type_arguments.push_back(returned);
  }
}

}  // namespace

std::expected<ReflectedTypeDescription, ExecutionStatus> DescribeReflectedType(
    ReflectedType type,
    const ReflectionIndex* index,
    TypeDescriptionDetail detail) {
  const auto kind = ReflectedKind(type);
  constexpr auto kNames = std::to_array<std::string_view>({
      "void",
      "Null",
      "dynamic",
      "bool",
      "int",
      "double",
      "string",
      "MutableString",
      "Array",
      "Set",
      "Map",
      "Function",
      "Class",
      "Interface",
      "Enum",
      "Future",
      "Completer",
      "Timer",
      "CancellationToken",
      "ByteArray",
      "ByteData",
      "RegExp",
      "RegExpMatch",
  });
  static_assert(kNames.size() == static_cast<std::size_t>(reflection::TypeKind::RegExpMatch) + 1);
  const auto& constraint = type.constraint;
  const auto is_nominal = constraint.nominal != nullptr || constraint.interface != nullptr;
  ReflectedTypeDescription result{
      .name = is_nominal ? std::string{} : std::string(kNames[static_cast<std::size_t>(kind)]),
      .module = {},
      .kind = kind,
      .is_nullable = type.constraint.nullable && kind != reflection::TypeKind::Null &&
                     kind != reflection::TypeKind::Dynamic,
      .type_arguments = {},
      .base_type = {},
      .interfaces = {},
      .signature = {},
  };
  const auto* nominal = index == nullptr ? nullptr : index->Type(type);
  if (nominal != nullptr) {
    result.is_registered = (nominal->flags & reflection::kRegistered) != 0;
  }
  if (is_nominal) {
    if (nominal == nullptr) {
      return std::unexpected(index == nullptr ? ExecutionStatus::InvalidEntry
                                              : ExecutionStatus::TargetDeleted);
    }
    result.name = nominal->name.text();
    result.module = nominal->module.text();
    if (detail == TypeDescriptionDetail::Identity) {
      return result;
    }
    for (const auto argument : nominal->arguments.values()) {
      result.type_arguments.push_back(index->TypeAt(argument));
    }
    if (nominal->base != kNoNativeIndex) {
      result.base_type = index->TypeAt(nominal->base);
    }
    for (const auto interface : nominal->interfaces.values()) {
      result.interfaces.push_back(index->TypeAt(interface));
    }
  } else if (detail == TypeDescriptionDetail::Identity) {
    return result;
  } else if (constraint.array != nullptr) {
    result.type_arguments.push_back({.constraint = constraint.array->value_type()});
  } else if (constraint.collection != nullptr) {
    result.type_arguments.push_back({.constraint = constraint.collection->keys.value_type()});
    if (constraint.collection->values) {
      result.type_arguments.push_back({.constraint = constraint.collection->values->value_type()});
    }
  } else if (constraint.signature != nullptr) {
    DescribeSignature(result, constraint);
  }
  return result;
}

}  // namespace vela::runtime
