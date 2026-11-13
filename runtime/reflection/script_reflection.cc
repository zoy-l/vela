#include "runtime/reflection/script_reflection.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/functional/function_ref.h>

#include "base/builtin.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/reflection.h"
#include "base/reflection_operation.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/isolate/isolate.h"
#include "runtime/isolate/layout_transaction.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/reflection/reflection_access.h"
#include "runtime/reflection/reflection_description.h"
#include "runtime/reflection/reflection_handle.h"
#include "runtime/reflection/reflection_index.h"
#include "runtime/reflection/reflection_lookup_cache.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

using reflection::MemberKind;
using reflection::Operation;
using ResultValue = std::expected<Value, ExecutionStatus>;

enum class Descriptor : std::uint8_t {
  Type,
  TypeId,
  MemberId,
  Field,
  Property,
  Method,
  Constructor,
  EnumMember,
  Parameter,
  Signature,
  SignatureParameter,
  StaticField,
  StaticProperty,
  StaticMethod,
};

constexpr auto kDescriptorNames = std::to_array<std::string_view>({
    reflection::kTypeInfoName,
    "TypeId",
    "MemberId",
    "FieldInfo",
    "PropertyInfo",
    "MethodInfo",
    "ConstructorInfo",
    "EnumMemberInfo",
    "ParameterInfo",
    "SignatureInfo",
    "SignatureParameterInfo",
    "StaticFieldInfo",
    "StaticPropertyInfo",
    "StaticMethodInfo",
});
static_assert(kDescriptorNames.size() == static_cast<std::size_t>(Descriptor::StaticMethod) + 1);
constexpr auto kMemberDescriptors = std::to_array<Descriptor>({
    Descriptor::Field,
    Descriptor::Property,
    Descriptor::Method,
    Descriptor::Method,
    Descriptor::Method,
    Descriptor::Constructor,
    Descriptor::EnumMember,
});
static_assert(kMemberDescriptors.size() == static_cast<std::size_t>(MemberKind::EnumMember) + 1);

MemberKind Category(Operation operation) {
  switch (operation) {
    case Operation::GetFields:
    case Operation::FindField:
    case Operation::GetStaticFields:
    case Operation::FindStaticField:
      return MemberKind::Field;
    case Operation::GetProperties:
    case Operation::FindProperty:
    case Operation::GetStaticProperties:
    case Operation::FindStaticProperty:
      return MemberKind::Property;
    case Operation::GetMethods:
    case Operation::FindMethod:
    case Operation::GetStaticMethods:
    case Operation::FindStaticMethod:
      return MemberKind::Method;
    case Operation::GetConstructor:
      return MemberKind::Constructor;
    default:
      return MemberKind::EnumMember;
  }
}

const ReflectionHandle* DescriptorHandle(Value value) {
  const auto* layout = value.IsObject() ? Heap::ClassOf(*value.AsObject()) : nullptr;
  return layout != nullptr && layout->IsMetadata() && layout->field_count() == 1
             ? AsReflectionHandle(Heap::LoadField(*value.AsObject(), 0))
             : nullptr;
}

bool IsMemberHandle(const ReflectionHandle& handle) noexcept {
  return handle.member != DeclarationId{};
}

std::expected<std::vector<ReflectedType>, ExecutionStatus> TypeArguments(Value value) {
  std::vector<ReflectedType> result;
  if (value.IsNull()) {
    return result;
  }
  const auto count = Heap::ArrayLength(*value.AsObject());
  result.reserve(*count);
  for (std::uint32_t index = 0; index < *count; ++index) {
    const auto* handle = DescriptorHandle(*Heap::GetElement(*value.AsObject(), index));
    if (handle == nullptr || IsMemberHandle(*handle)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    result.push_back(handle->type);
  }
  return result;
}

reflection::MemberQuery Query(Value value) {
  if (value.IsNull()) {
    return {};
  }
  const auto& object = *value.AsObject();
  return {
      .scope = static_cast<reflection::MemberScope>(Heap::LoadField(object, 0).AsInteger()),
      .visibility =
          static_cast<reflection::VisibilityFilter>(Heap::LoadField(object, 1).AsInteger()),
  };
}

class ScriptReflection final {
 public:
  ScriptReflection(NativeExecution& execution, ReflectionIndex& index, TypeConstraint result)
      : execution_(&execution),
        index_(&index),
        heap_(&execution.isolate().heap()),
        result_(result) {}
  ResultValue Type(ReflectedType type, const TypeConstraint* contract = nullptr);
  ResultValue Execute(Operation operation, std::span<const Value> arguments);
  ResultValue FindMethods(ReflectedType attribute, const TypeConstraint& result, Value module);

 private:
  const TypeConstraint& Contract(Descriptor descriptor);
  ResultValue Wrap(Descriptor descriptor, Value payload, const TypeConstraint* requested = nullptr);
  ResultValue Handle(Descriptor descriptor,
                     ReflectedType type,
                     const NativeReflectionMember* member,
                     const TypeConstraint* contract = nullptr);
  ResultValue Member(const NativeReflectionMember& member,
                     const TypeConstraint* contract = nullptr);
  ResultValue Array(Descriptor element,
                    std::size_t count,
                    absl::FunctionRef<ResultValue(std::size_t)> item,
                    const TypeConstraint* requested = nullptr);
  ResultValue Types(std::span<const ReflectedType> types);
  ResultValue Record(Descriptor descriptor, std::span<const Value> fields);
  // fields 由调用方保根；返回的 payload 在后续分配前由调用方保根。
  ResultValue ValueSlots(std::span<const Value> fields);
  ResultValue Parameters(const NativeReflectionMember& member);
  ResultValue Signature(const ReflectedSignature& signature);
  ResultValue Describe(Operation operation, const ReflectionHandle& handle);
  ResultValue DescribeMember(Operation operation,
                             const ReflectionHandle& handle,
                             const NativeReflectionMember& member);
  ResultValue Members(Operation operation, ReflectedType type, std::span<const Value> arguments);
  ResultValue ExecuteTarget(Operation operation, std::span<const Value> arguments);
  ResultValue Text(std::string_view text);
  ResultValue CopyAttribute(const NativeAttribute& attribute);
  NativeExecution* execution_;
  ReflectionIndex* index_;
  Heap* heap_;
  TypeConstraint result_;
  std::array<const TypeConstraint*, kDescriptorNames.size()> contracts_{};
};

template <class Action>
ResultValue WithReflection(NativeExecution& execution, HeapObject& state, const Action& action) {
  const auto execute = [&] -> ResultValue {
    const auto index = PrepareReflectionIndex(execution.instance());
    if (!index) {
      return std::unexpected(index.error());
    }
    return action(**index);
  };
  if (&state == execution.instance().state().front().AsObject()) {
    return execute();
  }
  auto current = NativeInstance::Borrow(state);
  const NativeInstanceScope scope(execution, current);
  return execute();
}

template <class Action>
ResultValue WithScriptReflection(NativeExecution& execution,
                                 HeapObject& state,
                                 const TypeConstraint& result,
                                 Action&& action) {
  return WithReflection(execution, state, [&](auto& index) -> ResultValue {
    ScriptReflection reflection(execution, index, result);
    return std::forward<Action>(action)(reflection);
  });
}

ResultValue ScriptReflection::Text(std::string_view text) {
  auto* string = heap_->AllocateString(text);
  return string == nullptr ? ResultValue(std::unexpected(ExecutionStatus::OutOfMemory))
                           : Value::String(string);
}

const TypeConstraint& ScriptReflection::Contract(Descriptor descriptor) {
  const auto position = static_cast<std::size_t>(descriptor);
  auto& contract = contracts_[position];
  if (contract == nullptr) {
    const std::array dynamic{ReflectedType{.constraint = {.dynamic = true}}};
    const auto generic = descriptor == Descriptor::Type || descriptor == Descriptor::Field ||
                         descriptor == Descriptor::Property || descriptor == Descriptor::Method;
    const auto* type =
        index_->FindDescribedType(reflection::kModule, kDescriptorNames[position],
                                  generic ? std::span(dynamic) : std::span<const ReflectedType>{});
    contract = &execution_->instance().layouts().types[type->type];
  }
  return *contract;
}

ResultValue ScriptReflection::Wrap(Descriptor descriptor,
                                   Value payload,
                                   const TypeConstraint* requested) {
  const RootRange root(*heap_, &payload, 1, std::nullopt);
  const auto& contract = requested != nullptr ? *requested : Contract(descriptor);
  const std::array fields{payload};
  auto* object = heap_->AllocateInstance(*contract.nominal->fixed_layout(), fields);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return Value::Object(object);
}

ResultValue ScriptReflection::Handle(Descriptor descriptor,
                                     ReflectedType type,
                                     const NativeReflectionMember* member,
                                     const TypeConstraint* contract) {
  const auto& instance = execution_->instance();
  const ReflectionHandle description{
      .type = type,
      .member = member != nullptr ? member->declaration : DeclarationId{},
      .anchor = instance.context_anchor(),
      .dispatch = instance.dispatch(),
      .cached_member = member,
      .cached_revision = instance.current_revision(),
      .field_cache = {},
      .call_cache = {},
      .lookup_cache = {},
  };
  auto* handle = heap_->AllocateReflectionHandle(description);
  return handle == nullptr ? ResultValue(std::unexpected(ExecutionStatus::OutOfMemory))
                           : Wrap(descriptor, Value::Object(handle), contract);
}

ResultValue ScriptReflection::Type(ReflectedType type, const TypeConstraint* contract) {
  return Handle(Descriptor::Type, type, nullptr, contract);
}

ResultValue ScriptReflection::CopyAttribute(const NativeAttribute& attribute) {
  const auto& instance = execution_->instance();
  const auto& image = instance.image();
  const auto& layouts = instance.layouts();
  const auto type = image.types.values()[attribute.type];
  const auto declaration_fields = image.classes.values()[type.index].fields.values();
  const auto fields = attribute.fields.values();
  NativeBuffer<Value> storage(*heap_, fields.size());
  if (!storage.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto values = storage.values();
  std::ranges::fill(values, Value::Null());
  const RootRange roots(*heap_, values.data(), values.size(), std::nullopt);
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto& field = fields[index];
    switch (static_cast<NativeAttributeValueKind>(field.kind)) {
      case NativeAttributeValueKind::Null:
        break;
      case NativeAttributeValueKind::Integer:
        values[index] = Value::Integer(std::bit_cast<std::int64_t>(field.payload));
        break;
      case NativeAttributeValueKind::Double:
        values[index] = Value::Double(std::bit_cast<double>(field.payload));
        break;
      case NativeAttributeValueKind::Boolean:
        values[index] = Value::Boolean(field.payload != 0);
        break;
      case NativeAttributeValueKind::String: {
        const auto text = Text(field.text.text());
        if (!text) {
          return text;
        }
        values[index] = *text;
        break;
      }
      default:
        std::unreachable();
    }
    if (layouts.types[declaration_fields[index].type].dynamic) {
      const auto boxed = BoxDynamicValue(*heap_, values[index], layouts.types[field.type]);
      if (!boxed) {
        return boxed;
      }
      values[index] = *boxed;
    }
  }
  auto* object = heap_->AllocateInstance(*layouts.classes[type.index], values);
  return object != nullptr ? ResultValue(Value::Object(object))
                           : std::unexpected(ExecutionStatus::OutOfMemory);
}

ResultValue ScriptReflection::FindMethods(ReflectedType attribute,
                                          const TypeConstraint& result,
                                          Value module) {
  const auto filter = module.IsNull() ? std::nullopt : std::optional(module.AsString()->value());
  struct Match final {
    const NativeAttribute* attribute;
    const NativeReflectionMember* member;
  };
  std::vector<Match> matches;
  index_->EnumerateMethodAttributes(
      attribute, filter, [&](const auto& annotation, const auto& member) {
        matches.push_back({.attribute = &annotation, .member = &member});
      });
  auto* array = heap_->AllocateArray(*result.array, static_cast<std::uint32_t>(matches.size()));
  if (array == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto value = Value::Object(array);
  const RootRange root(*heap_, &value, 1, std::nullopt);
  std::uint32_t index = 0;
  for (const auto& selected : matches) {
    std::array<Value, 4> pair;
    const RootRange pair_roots(*heap_, pair.data(), pair.size(), std::nullopt);
    const auto method = Member(*selected.member);
    if (!method) {
      return method;
    }
    pair.front() = *method;
    const auto data = CopyAttribute(*selected.attribute);
    if (!data) {
      return data;
    }
    pair[1] = *data;
    pair[(selected.member->flags & reflection::kStatic) != 0 ? 3 : 2] = *method;
    const auto payload = ValueSlots(pair);
    if (!payload) {
      return payload;
    }
    const std::array initial{*payload};
    const RootRange payload_root(*heap_, initial.data(), initial.size(), std::nullopt);
    auto* match = heap_->AllocateInstance(*result.array->element.nominal->fixed_layout(), initial);
    if (match == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    std::ignore = heap_->SetElement(*array, index++, Value::Object(match));
  }
  return value;
}

ResultValue ScriptReflection::Member(const NativeReflectionMember& member,
                                     const TypeConstraint* contract) {
  auto descriptor = kMemberDescriptors[member.kind];
  if ((member.flags & reflection::kStatic) != 0) {
    if (descriptor == Descriptor::Field) {
      descriptor = Descriptor::StaticField;
    } else if (descriptor == Descriptor::Property) {
      descriptor = Descriptor::StaticProperty;
    } else if (descriptor == Descriptor::Method) {
      descriptor = Descriptor::StaticMethod;
    }
  }
  return Handle(descriptor, index_->TypeAt(member.declaring_type), &member, contract);
}

ResultValue ScriptReflection::Array(Descriptor element,
                                    std::size_t count,
                                    absl::FunctionRef<ResultValue(std::size_t)> item,
                                    const TypeConstraint* requested) {
  const auto constraint = requested != nullptr ? *requested : Contract(element);
  LayoutTransaction transaction(execution_->isolate(), LayoutLifetime::Collectible);
  const auto* layout = execution_->isolate().CreateArrayLayout({.element = constraint});
  auto* array = heap_->AllocateArray(*layout, static_cast<std::uint32_t>(count));
  if (array == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto result = Value::Object(array);
  const RootRange root(*heap_, &result, 1, std::nullopt);
  transaction.CommitStructural();
  for (std::size_t position = 0; position < count; ++position) {
    const auto value = item(position);
    if (!value) {
      return value;
    }
    std::ignore = heap_->SetElement(*array, static_cast<std::uint32_t>(position), *value);
  }
  return result;
}

ResultValue ScriptReflection::Types(std::span<const ReflectedType> types) {
  return Array(Descriptor::Type, types.size(),
               [&](auto position) { return Type(types[position]); });
}

ResultValue ScriptReflection::ValueSlots(std::span<const Value> fields) {
  auto* values = heap_->AllocateValueSlots(fields.size());
  if (values == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  for (std::size_t index = 0; index < fields.size(); ++index) {
    heap_->SetValueSlot(*values, index, fields[index]);
  }
  return Value::Object(values);
}

ResultValue ScriptReflection::Record(Descriptor descriptor, std::span<const Value> fields) {
  const RootRange roots(*heap_, fields.data(), fields.size(), std::nullopt);
  const auto values = ValueSlots(fields);
  return values ? Wrap(descriptor, *values) : values;
}

ResultValue ScriptReflection::Parameters(const NativeReflectionMember& member) {
  const auto parameters = member.parameters.values();
  return Array(Descriptor::Parameter, parameters.size(), [&](auto position) -> ResultValue {
    const auto& parameter = parameters[position];
    constexpr std::size_t kParameterFieldCount = 6;
    constexpr std::size_t kHasDefaultField = kParameterFieldCount - 1;
    std::array<Value, kParameterFieldCount> fields;
    const RootRange roots(*heap_, fields.data(), fields.size(), std::nullopt);
    const auto name = Text(parameter.name.text());
    if (!name) {
      return name;
    }
    fields[0] = *name;
    fields[1] = Value::Integer(static_cast<std::int64_t>(position));
    const auto type = Type(index_->TypeAt(parameter.type));
    if (!type) {
      return type;
    }
    fields[2] = *type;
    fields[3] = Value::Boolean(parameter.IsOptional());
    fields[4] = Value::Boolean(parameter.IsRest());
    fields[kHasDefaultField] = Value::Boolean(parameter.HasDefault());
    return Record(Descriptor::Parameter, fields);
  });
}

ResultValue ScriptReflection::Signature(const ReflectedSignature& signature) {
  std::array<Value, 2> fields;
  const RootRange roots(*heap_, fields.data(), fields.size(), std::nullopt);
  const auto parameters = Array(Descriptor::SignatureParameter, signature.parameters.size(),
                                [&](auto position) -> ResultValue {
                                  const auto& parameter = signature.parameters[position];
                                  const auto type = Type(parameter.type);
                                  if (!type) {
                                    return type;
                                  }
                                  const std::array values{
                                      *type,
                                      Value::Boolean(parameter.is_optional),
                                      Value::Boolean(parameter.is_rest),
                                  };
                                  return Record(Descriptor::SignatureParameter, values);
                                });
  if (!parameters) {
    return parameters;
  }
  fields[0] = *parameters;
  const auto returned = Type(signature.return_type);
  if (!returned) {
    return returned;
  }
  fields[1] = *returned;
  return Record(Descriptor::Signature, fields);
}

ResultValue ScriptReflection::Describe(Operation operation, const ReflectionHandle& handle) {
  const auto detail = operation >= Operation::TypeArguments && operation <= Operation::Signature
                          ? TypeDescriptionDetail::Complete
                          : TypeDescriptionDetail::Identity;
  const auto described = DescribeReflectedType(handle.type, index_, detail);
  if (!described) {
    return std::unexpected(described.error());
  }
  const auto& type = *described;
  switch (operation) {
    case Operation::Name:
      return Text(type.name);
    case Operation::Module:
      return type.module ? Text(*type.module) : Value::Null();
    case Operation::Kind:
      return Value::Integer(static_cast<std::int64_t>(type.kind));
    case Operation::IsNullable:
      return Value::Boolean(type.is_nullable);
    case Operation::IsRegistered:
      return Value::Boolean(type.is_registered);
    case Operation::TypeArguments:
      return Types(type.type_arguments);
    case Operation::BaseType:
      return type.base_type ? Type(*type.base_type) : Value::Null();
    case Operation::Interfaces:
      return Types(type.interfaces);
    case Operation::Signature:
      return type.signature ? Signature(*type.signature) : Value::Null();
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
}

ResultValue ScriptReflection::DescribeMember(Operation operation,
                                             const ReflectionHandle& handle,
                                             const NativeReflectionMember& member) {
  switch (operation) {
    case Operation::Name:
      return Text(member.name.text());
    case Operation::Kind:
      return Value::Integer(member.kind);
    case Operation::DeclaringType:
      return Type(handle.type);
    case Operation::Visibility:
      return Value::Integer((member.flags & reflection::kPrivate) != 0 ? 1 : 0);
    case Operation::IsStatic:
      return Value::Boolean((member.flags & reflection::kStatic) != 0);
    case Operation::ValueType:
    case Operation::ReturnType:
      return Type(index_->TypeAt(member.type));
    case Operation::IsAsync:
      return Value::Boolean((member.flags & reflection::kAsync) != 0);
    case Operation::Ordinal:
      return Value::Integer(member.target);
    case Operation::EnumValue:
      return ReadReflectedEnumMember(*execution_, member.declaration);
    case Operation::Parameters:
      return Parameters(member);
    case Operation::TypeArguments: {
      std::vector<ReflectedType> types;
      const auto arguments = member.type_arguments.values();
      types.reserve(arguments.size());
      for (const auto argument : arguments) {
        types.push_back(index_->TypeAt(argument));
      }
      return Types(types);
    }
    case Operation::CanRead:
    case Operation::CanWrite:
    case Operation::Getter:
    case Operation::Setter: {
      const auto getter = operation == Operation::Getter || operation == Operation::CanRead;
      const auto kind = getter ? MemberKind::Getter : MemberKind::Setter;
      if (operation == Operation::Getter || operation == Operation::Setter) {
        const auto* accessor = index_->RegisteredAccessor(member, kind);
        return accessor != nullptr ? Member(*accessor, &result_) : Value::Null();
      }
      return Value::Boolean(index_->CanAccess(member, kind));
    }
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
}

ResultValue ScriptReflection::Members(Operation operation,
                                      ReflectedType type,
                                      std::span<const Value> arguments) {
  const auto single =
      operation == Operation::FindField || operation == Operation::FindProperty ||
      operation == Operation::FindMethod || operation == Operation::FindStaticField ||
      operation == Operation::FindStaticProperty || operation == Operation::FindStaticMethod ||
      operation == Operation::GetConstructor || operation == Operation::FindEnumMember;
  const auto kind = Category(operation);
  const auto storage = kind == MemberKind::EnumMember || (operation >= Operation::GetStaticFields &&
                                                          operation <= Operation::FindStaticMethod)
                           ? reflection::MemberStorage::Static
                           : reflection::MemberStorage::Instance;
  if (single) {
    const auto constructor = kind == MemberKind::Constructor;
    const auto enumeration = kind == MemberKind::EnumMember;
    const auto name = constructor ? reflection::kConstructorName : arguments[0].AsString()->value();
    reflection::MemberQuery query;
    if (constructor) {
      query = reflection::kConstructorQuery;
    } else if (!enumeration) {
      query = Query(arguments[1]);
      query.storage = storage;
    }
    const auto parameters =
        operation == Operation::FindMethod || operation == Operation::FindStaticMethod
            ? TypeArguments(arguments[2])
            : std::expected<std::vector<ReflectedType>, ExecutionStatus>(
                  std::vector<ReflectedType>{});
    if (!parameters) {
      return std::unexpected(parameters.error());
    }
    const auto found = index_->FindMember(type, kind, name, query, *parameters);
    if (!found) {
      return std::unexpected(found.error());
    }
    return *found == nullptr ? ResultValue(Value::Null()) : Member(**found, &result_);
  }
  std::vector<const NativeReflectionMember*> members;
  auto query = kind == MemberKind::EnumMember ? reflection::MemberQuery{} : Query(arguments[0]);
  query.storage = storage;
  const auto enumerated = index_->EnumerateMembers(
      type, kind, query, [&](const auto& member) { members.push_back(&member); });
  if (!enumerated) {
    return std::unexpected(enumerated.error());
  }
  return Array(
      kMemberDescriptors[static_cast<std::size_t>(kind)], members.size(),
      [&](auto position) { return Member(*members[position], &result_.array->element); },
      &result_.array->element);
}

ResultValue InvokeScriptMember(NativeExecution& execution,
                               const NativeReflectionMember& member,
                               Value receiver,
                               const HeapObject& arguments,
                               const ReflectionHandle& handle) {
  const auto& items = *Heap::LoadField(arguments, 0).AsObject();
  return InvokeScriptReflectedMember(execution, member, receiver, items, handle);
}

bool IsMemberExecution(Operation operation) {
  switch (operation) {
    case Operation::GetValue:
    case Operation::SetValue:
    case Operation::GetStaticValue:
    case Operation::SetStaticValue:
    case Operation::Invoke:
    case Operation::InvokeStatic:
    case Operation::Construct:
      return true;
    default:
      return false;
  }
}

ResultValue ExecuteMember(NativeExecution& execution,
                          const NativeReflectionMember& member,
                          Operation operation,
                          std::span<const Value> arguments,
                          const ReflectionHandle& handle) {
  switch (operation) {
    case Operation::GetValue:
      return ReadReflectedMember(execution, member, arguments[1]);
    case Operation::SetValue:
      return WriteReflectedMember(execution, member, arguments[1], arguments[2]).transform([] {
        return Value::Null();
      });
    case Operation::GetStaticValue:
      return ReadReflectedMember(execution, member, Value::Null());
    case Operation::SetStaticValue:
      return WriteReflectedMember(execution, member, Value::Null(), arguments[1]).transform([] {
        return Value::Null();
      });
    case Operation::Invoke:
      return InvokeScriptMember(execution, member, arguments[1], *arguments[2].AsObject(), handle);
    case Operation::InvokeStatic:
    case Operation::Construct:
      return InvokeScriptMember(execution, member, Value::Null(), *arguments[1].AsObject(), handle);
    default:
      std::unreachable();
  }
}

template <class Action>
ResultValue WithResolvedMember(NativeExecution& execution,
                               const ReflectionHandle& handle,
                               Action&& action) {
  const auto program = ReflectionProgram(handle);
  if (!program) {
    return std::unexpected(program.error());
  }
  return WithReflection(execution, **program, [&](auto& index) -> ResultValue {
    const auto member = handle.ResolveMember(index, execution.instance().current_revision());
    if (!member) {
      return std::unexpected(member.error());
    }
    return std::forward<Action>(action)(**member);
  });
}

ResultValue ExecuteMemberOperation(NativeExecution& execution,
                                   Operation operation,
                                   std::span<const Value> arguments) {
  const auto* handle = DescriptorHandle(arguments[0]);
  if (handle == nullptr || !IsMemberHandle(*handle)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return WithResolvedMember(execution, *handle, [&](const auto& member) -> ResultValue {
    const auto* dispatch = execution.instance().dispatch();
    const auto epoch = dispatch != nullptr ? dispatch->publication_epoch() : 0;
    const auto result = ExecuteMember(execution, member, operation, arguments, *handle);
    if (result && (dispatch == nullptr || dispatch->publication_epoch() == epoch)) {
      handle->CacheField(execution.instance(), member, arguments[1]);
    }
    return result;
  });
}

ResultValue ScriptReflection::Execute(Operation operation, std::span<const Value> arguments) {
  const auto subject = arguments[0];
  if (operation == Operation::TypeOf) {
    return Type({.constraint = Heap::TypeOf(subject)}, &result_);
  }
  if (operation == Operation::VoidType) {
    return Type({.constraint = {}, .is_void = true}, &result_);
  }
  if (operation == Operation::GetTypes) {
    std::vector<ReflectedType> types;
    index_->EnumerateTypes(subject.AsString()->value(),
                           [&](const auto& type) { types.push_back(index_->TypeAt(type.type)); });
    return Types(types);
  }
  if (operation == Operation::FindType) {
    const auto types = TypeArguments(arguments[2]);
    if (!types) {
      return std::unexpected(types.error());
    }
    const auto* type =
        index_->FindType(subject.AsString()->value(), arguments[1].AsString()->value(), *types);
    return type == nullptr ? ResultValue(Value::Null())
                           : Type(index_->TypeAt(type->type), &result_);
  }
  if (operation == Operation::Revision) {
    return Value::Integer(static_cast<std::int64_t>(execution_->instance().current_revision()));
  }
  if (operation >= Operation::RecordItem && operation < Operation::FindAttributedMethods) {
    const auto data = Heap::LoadField(*subject.AsObject(), 0);
    return Heap::ValueSlots(*data.AsObject())[static_cast<std::size_t>(operation) -
                                              static_cast<std::size_t>(Operation::RecordItem)];
  }
  return ExecuteTarget(operation, arguments);
}

ResultValue ScriptReflection::ExecuteTarget(Operation operation, std::span<const Value> arguments) {
  const auto subject = arguments[0];
  const auto* handle = DescriptorHandle(subject);
  if (handle == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto is_member = IsMemberHandle(*handle);
  if (operation == Operation::Id) {
    return Wrap(is_member ? Descriptor::MemberId : Descriptor::TypeId,
                Heap::LoadField(*subject.AsObject(), 0));
  }
  if (operation == Operation::IsInstance) {
    return Value::Boolean(handle->type.IsInstance(arguments[1]));
  }
  if (operation == Operation::IsAssignableFrom) {
    const auto* other = DescriptorHandle(arguments[1]);
    const auto result = handle->type.IsAssignableFrom(other->type);
    if (!result) {
      return std::unexpected(result.error());
    }
    return Value::Boolean(*result);
  }
  if ((operation >= Operation::GetFields && operation <= Operation::FindEnumMember) ||
      (operation >= Operation::GetStaticFields && operation <= Operation::FindStaticMethod)) {
    return Members(operation, handle->type, arguments.subspan(1));
  }
  if (!is_member) {
    return Describe(operation, *handle);
  }
  const auto member = handle->ResolveMember(*index_, execution_->instance().current_revision());
  if (!member) {
    return std::unexpected(member.error());
  }
  return DescribeMember(operation, *handle, **member);
}

struct LookupSelection final {
  ReflectedLookupCache* cache = nullptr;
  HeapObject* owner = nullptr;
  Value qualifier;
  Value name;
};

std::optional<LookupSelection> SelectLookupCache(NativeInstance& instance,
                                                 Operation operation,
                                                 std::span<const Value> arguments) {
  if (operation == Operation::FindType && arguments[2].IsNull()) {
    return LookupSelection{
        .cache = &instance.program().reflection_type_cache,
        .owner = &instance.current_state(),
        .qualifier = arguments[0],
        .name = arguments[1],
    };
  }
  const auto method =
      operation == Operation::FindMethod || operation == Operation::FindStaticMethod;
  if (!IsCachedMemberLookup(operation) || !arguments[2].IsNull() ||
      (method && !arguments[3].IsNull())) {
    return std::nullopt;
  }
  const auto* handle = DescriptorHandle(arguments[0]);
  if (handle == nullptr || IsMemberHandle(*handle)) {
    return std::nullopt;
  }
  return LookupSelection{
      .cache = &handle->lookup_cache,
      .owner = Heap::LoadField(*arguments[0].AsObject(), 0).AsObject(),
      .qualifier = {},
      .name = arguments[1],
  };
}

ResultValue ExecuteMetadataLookup(NativeExecution& execution,
                                  ScriptReflection& reflection,
                                  Operation operation,
                                  std::span<const Value> arguments,
                                  std::uint32_t result_type,
                                  const NativeProgramState& caller) {
  const auto& instance = execution.instance();
  const auto& program = instance.program();
  const auto selected = SelectLookupCache(execution.instance(), operation, arguments);
  if (!selected || &caller != &program) {
    return reflection.Execute(operation, arguments);
  }
  auto& cache = *selected->cache;
  const auto* dispatch = instance.dispatch();
  const auto* epoch = dispatch != nullptr ? &dispatch->publication_epoch() : &cache.observed;
  const auto observed = *epoch;
  const auto encoded = static_cast<std::uint32_t>(operation);
  if (cache.epoch == epoch && cache.observed == observed && cache.program == &program &&
      cache.operation == encoded && cache.result_type == result_type &&
      cache.name.AsString() == selected->name.AsString() &&
      cache.qualifier.AsString() == selected->qualifier.AsString()) {
    return cache.result;
  }
  const auto result = reflection.Execute(operation, arguments);
  if (result && *epoch == observed) {
    cache.program = &program;
    cache.operation = encoded;
    cache.result_type = result_type;
    cache.qualifier = selected->qualifier;
    cache.name = selected->name;
    cache.result = *result;
    cache.observed = observed;
    cache.epoch = epoch;
    auto& heap = execution.isolate().heap();
    heap.WriteBarrier(*selected->owner, cache.qualifier);
    heap.WriteBarrier(*selected->owner, cache.name);
    heap.WriteBarrier(*selected->owner, cache.result);
  }
  return result;
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteReflectedInvocation(
    NativeExecution& execution,
    std::span<const NativeValue> snapshot) {
  const auto* handle = DescriptorHandle(DecodeNativeValue(snapshot[1]));
  if (handle == nullptr || !IsMemberHandle(*handle)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return WithResolvedMember(execution, *handle, [&](const auto& member) -> ResultValue {
    return InvokeScriptReflectedMember(execution, member, DecodeNativeValue(snapshot[2]),
                                       snapshot.subspan(kReflectionInvokeHeaderArity), *handle);
  });
}

std::expected<Value, ExecutionStatus> ExecuteNativeReflection(NativeExecution& execution,
                                                              Builtin builtin,
                                                              std::span<const Value> arguments) {
  auto& isolate = execution.isolate();
  const RootRange roots(isolate.heap(), arguments.data(), arguments.size(), std::nullopt);
  if (const auto status = isolate.native_control().Poll(); status != ExecutionStatus::Returned) {
    return std::unexpected(status);
  }
  if (builtin == Builtin::ReflectionTypeInfo) {
    const auto type_index = static_cast<std::uint32_t>(arguments[0].AsInteger());
    const ReflectedType type{.constraint = execution.instance().layouts().types[type_index]};
    const RootRange type_root(isolate.heap(), std::span(&type.constraint, 1));
    const auto result =
        execution.instance().layouts().types[static_cast<std::uint32_t>(arguments[1].AsInteger())];
    return WithScriptReflection(execution, execution.instance().current_state(), result,
                                [&](auto& reflection) { return reflection.Type(type, &result); });
  }
  const auto decoded = DecodeOperation<Operation::InvokeStatic>(arguments[0]);
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  const auto operation = *decoded;
  if (IsMemberExecution(operation)) {
    return ExecuteMemberOperation(execution, operation, arguments.subspan(1));
  }
  const auto result_index = static_cast<std::uint32_t>(arguments.back().AsInteger());
  const auto result = result_index == kNoNativeIndex
                          ? TypeConstraint{}
                          : execution.instance().layouts().types[result_index];
  const RootRange result_root(isolate.heap(), std::span(&result, 1));
  if (operation == Operation::FindAttributedMethods) {
    const auto& types = execution.instance().layouts().types;
    const std::array constraints{
        types[static_cast<std::uint32_t>(arguments[1].AsInteger())],
        types[static_cast<std::uint32_t>(arguments[2].AsInteger())],
    };
    const RootRange type_roots(isolate.heap(), constraints);
    return WithScriptReflection(execution, execution.instance().current_state(), result,
                                [&](auto& reflection) {
                                  return reflection.FindMethods({.constraint = constraints.front()},
                                                                constraints.back(), arguments[3]);
                                });
  }
  HeapObject* state = nullptr;
  if (const auto* handle = DescriptorHandle(arguments[1])) {
    const auto program = ReflectionProgram(*handle);
    if (!program) {
      return std::unexpected(program.error());
    }
    state = *program;
  } else {
    state = &execution.instance().current_state();
  }
  const auto& caller = execution.instance().program();
  return WithScriptReflection(execution, *state, result, [&](auto& reflection) {
    return ExecuteMetadataLookup(execution, reflection, operation, arguments.subspan(1),
                                 result_index, caller);
  });
}

}  // namespace vela::runtime
