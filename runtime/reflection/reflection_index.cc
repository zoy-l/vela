#include "runtime/reflection/reflection_index.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <absl/functional/function_ref.h>
#include <absl/hash/hash.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/reflection.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

std::uint64_t TypeHash(ReflectedType type) {
  type.constraint.nullable = false;
  return absl::HashOf(type);
}

template <class Arguments, class Resolve>
std::uint64_t ArgumentHash(std::uint64_t seed, Arguments arguments, const Resolve& resolve) {
  for (const auto& argument : arguments) {
    seed = absl::HashOf(seed, resolve(argument));
  }
  return seed;
}

template <class Arguments, class Resolve>
std::uint64_t TypeLookupKey(std::string_view module,
                            std::string_view name,
                            Arguments arguments,
                            const Resolve& resolve) {
  return ArgumentHash(absl::HashOf(module, name), arguments, resolve);
}

template <class Arguments, class Resolve>
std::uint64_t MemberLookupKey(std::uint32_t kind,
                              std::string_view name,
                              Arguments arguments,
                              const Resolve& resolve) {
  return ArgumentHash(absl::HashOf(kind, name), arguments, resolve);
}

bool Matches(const NativeReflectionMember& member,
             reflection::MemberKind kind,
             reflection::MemberQuery query) {
  if (member.kind != static_cast<std::uint32_t>(kind) ||
      (member.flags & reflection::kRegistered) == 0) {
    return false;
  }
  const auto is_static = (member.flags & reflection::kStatic) != 0;
  if ((query.storage == reflection::MemberStorage::Instance && is_static) ||
      (query.storage == reflection::MemberStorage::Static && !is_static)) {
    return false;
  }
  const auto is_private = (member.flags & reflection::kPrivate) != 0;
  return query.visibility == reflection::VisibilityFilter::All ||
         ((query.visibility == reflection::VisibilityFilter::Private) == is_private);
}

}  // namespace

bool ReflectedType::IsInstance(Value value) const noexcept {
  return !is_void && Heap::IsValueOfType(value, constraint);
}

std::expected<bool, ExecutionStatus> ReflectedType::IsAssignableFrom(
    const ReflectedType& other) const noexcept {
  if (is_void || other.is_void) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return other.constraint.IsSubtypeOf(constraint);
}

ReflectionIndex::ReflectionIndex(Heap& heap,
                                 const NativeImage& image,
                                 std::span<const TypeConstraint> types)
    : memory_(heap, sizeof(ReflectionIndex)),
      heap_(&heap),
      image_(&image),
      types_(types),
      views_(heap, image.reflection_types.size),
      type_positions_(heap, image.reflection_types.size),
      type_constraints_(heap, image.reflection_types.size),
      type_names_(heap, image.reflection_types.size),
      member_ids_(heap, image.reflection_members.size),
      declared_(heap, image.reflection_members.size),
      signatures_(heap, image.reflection_members.size) {
  if (views_.IsAllocated()) {
    const auto views = views_.values();
    std::uninitialized_value_construct_n(views.data(), views.size());
  }
  if (signatures_.IsAllocated()) {
    const auto signatures = signatures_.values();
    std::uninitialized_value_construct_n(signatures.data(), signatures.size());
  }
}

ReflectionIndex::~ReflectionIndex() {
  heap_->FreeNative(attribute_lookup_);
  if (signatures_.IsAllocated()) {
    for (auto* signature : signatures_.values()) {
      if (signature != nullptr) {
        std::destroy_at(signature);
        heap_->FreeNative(signature);
      }
    }
  }
  if (views_.IsAllocated()) {
    for (const auto& view : views_.values()) {
      heap_->FreeNative(view.inherited);
      heap_->FreeNative(view.lookup);
    }
  }
}

bool ReflectionIndex::IsAllocated() const noexcept {
  return memory_.IsReserved() && views_.IsAllocated() && type_positions_.IsAllocated() &&
         type_constraints_.IsAllocated() && type_names_.IsAllocated() &&
         member_ids_.IsAllocated() && declared_.IsAllocated() && signatures_.IsAllocated();
}

std::expected<std::unique_ptr<ReflectionIndex>, ExecutionStatus> ReflectionIndex::Create(
    Heap& heap,
    const NativeImage& image,
    std::span<const TypeConstraint> types) {
  std::unique_ptr<ReflectionIndex> index(new (std::nothrow) ReflectionIndex(heap, image, types));
  if (index == nullptr || !index->IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto prepared = index->Prepare();
  if (!prepared) {
    return std::unexpected(prepared.error());
  }
  return index;
}

ReflectedType ReflectionIndex::TypeAt(std::uint32_t index) const noexcept {
  return index == kNoNativeIndex ? ReflectedType{.constraint = {}, .is_void = true}
                                 : ReflectedType{.constraint = types_[index]};
}

std::optional<std::uint32_t> ReflectionIndex::TypePosition(std::uint32_t index) const noexcept {
  const auto entries = type_positions_.values();
  const auto found = std::ranges::lower_bound(entries, index, {}, &Lookup::key);
  return found != entries.end() && found->key == index ? std::optional(found->index) : std::nullopt;
}

std::optional<std::uint32_t> ReflectionIndex::TypePosition(ReflectedType type) const noexcept {
  const auto entries = type_constraints_.values();
  const auto hash = TypeHash(type);
  const auto matches = std::ranges::equal_range(entries, hash, {}, &Lookup::key);
  type.constraint.nullable = false;
  for (const auto& entry : matches) {
    if (TypeAt(image_->reflection_types.values()[entry.index].type) == type) {
      return entry.index;
    }
  }
  return std::nullopt;
}

const NativeReflectionType* ReflectionIndex::Type(ReflectedType type) const noexcept {
  const auto position = TypePosition(type);
  return position ? &image_->reflection_types.values()[*position] : nullptr;
}

bool ReflectionIndex::MatchesArguments(std::span<const std::uint32_t> indices,
                                       std::span<const ReflectedType> arguments) const noexcept {
  return std::ranges::equal(indices, arguments, {}, [this](auto index) { return TypeAt(index); });
}

const NativeReflectionType* ReflectionIndex::FindType(
    std::string_view module,
    std::string_view name,
    std::span<const ReflectedType> arguments) const {
  const auto* type = FindDescribedType(module, name, arguments);
  return type != nullptr && (type->flags & reflection::kRegistered) != 0 ? type : nullptr;
}

const NativeReflectionType* ReflectionIndex::FindDescribedType(
    std::string_view module,
    std::string_view name,
    std::span<const ReflectedType> arguments) const {
  const auto hash =
      TypeLookupKey(module, name, arguments, [](const auto& argument) { return argument; });
  const auto candidates = std::ranges::equal_range(type_names_.values(), hash, {}, &Lookup::key);
  for (const auto& entry : candidates) {
    const auto& type = image_->reflection_types.values()[entry.index];
    if (type.module.text() == module && type.name.text() == name &&
        MatchesArguments(type.arguments.values(), arguments)) {
      return &type;
    }
  }
  return nullptr;
}

void ReflectionIndex::EnumerateTypes(
    std::string_view module,
    absl::FunctionRef<void(const NativeReflectionType&)> visit) const {
  const auto types = image_->reflection_types.values();
  const auto selected = std::ranges::equal_range(
      types, module, {}, [](const auto& type) { return type.module.text(); });
  for (const auto& type : selected) {
    if ((type.flags & reflection::kRegistered) != 0) {
      visit(type);
    }
  }
}

const NativeReflectionMember* ReflectionIndex::Member(DeclarationId declaration) const noexcept {
  const auto entries = member_ids_.values();
  const auto found = std::ranges::lower_bound(entries, declaration.value, {}, &Lookup::key);
  return found != entries.end() && found->key == declaration.value
             ? &image_->reflection_members.values()[found->index]
             : nullptr;
}

std::expected<const NativeReflectionMember*, ExecutionStatus> ReflectionIndex::ResolveMember(
    DeclarationId declaration) const noexcept {
  const auto* member = Member(declaration);
  if (member != nullptr && (member->flags & reflection::kRegistered) != 0) {
    return member;
  }
  const auto exists = FindDeclaration(image_->declarations.values(), declaration) != nullptr;
  return std::unexpected(exists ? ExecutionStatus::ReflectionUnavailable
                                : ExecutionStatus::TargetDeleted);
}

const NativeReflectionMember* ReflectionIndex::RegisteredAccessor(
    const NativeReflectionMember& property,
    reflection::MemberKind kind) const noexcept {
  const auto accessor = ResolveAccessor(property, kind);
  return accessor ? *accessor : nullptr;
}

std::expected<const NativeReflectionMember*, ExecutionStatus> ReflectionIndex::ResolveAccessor(
    const NativeReflectionMember& property,
    reflection::MemberKind kind) const noexcept {
  const auto declaration =
      kind == reflection::MemberKind::Getter ? property.getter : property.setter;
  return declaration != DeclarationId{} ? ResolveMember(declaration)
                                        : std::unexpected(ExecutionStatus::TypeError);
}

bool ReflectionIndex::CanAccess(const NativeReflectionMember& property,
                                reflection::MemberKind kind) const noexcept {
  const auto* accessor = RegisteredAccessor(property, kind);
  return accessor != nullptr && (accessor->flags & reflection::kPrivate) == 0;
}

std::expected<const CallSignature*, ExecutionStatus> ReflectionIndex::Signature(
    const NativeReflectionMember& member) {
  const auto offset = &member - image_->reflection_members.data;
  auto*& cached = signatures_.values()[static_cast<std::size_t>(offset)];
  if (cached != nullptr) {
    return cached;
  }
  return PrepareSignature(member, cached);
}

std::expected<const CallSignature*, ExecutionStatus> ReflectionIndex::PrepareSignature(
    const NativeReflectionMember& member,
    CallSignature*& cached) {
  auto bytes = signature_bytes_ + (member.parameters.size *
                                   (sizeof(CallSignature::Parameter) + sizeof(std::uint32_t)));
  for (const auto& parameter : member.parameters.values()) {
    bytes += parameter.name.size + 1;
  }
  if (const auto reserved = memory_.Resize(bytes); !reserved) {
    return std::unexpected(reserved.error());
  }
  CallSignature signature;
  signature.parameters.reserve(member.parameters.size);
  for (const auto& parameter : member.parameters.values()) {
    signature.parameters.push_back({
        .name = std::string(parameter.name.text()),
        .type = parameter.type,
        .optional = parameter.IsOptional(),
        .rest = parameter.IsRest(),
    });
  }
  if (member.type != kNoNativeIndex) {
    signature.result = member.type;
  }
  signature.IndexParameters();
  bytes = signature_bytes_ + (signature.parameters.capacity() * sizeof(CallSignature::Parameter)) +
          (signature.parameter_name_order.capacity() * sizeof(std::uint32_t));
  for (const auto& parameter : signature.parameters) {
    bytes += parameter.name.capacity() + 1;
  }
  if (const auto reserved = memory_.Resize(bytes); !reserved) {
    std::ignore = memory_.Resize(signature_bytes_);
    return std::unexpected(reserved.error());
  }
  auto* storage = static_cast<CallSignature*>(heap_->AllocateNative(sizeof(CallSignature)));
  if (storage == nullptr) {
    std::ignore = memory_.Resize(signature_bytes_);
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  signature_bytes_ = bytes;
  cached = std::construct_at(storage, std::move(signature));
  return cached;
}

std::expected<std::optional<std::uint32_t>, ExecutionStatus> ReflectionIndex::QueryType(
    ReflectedType type,
    reflection::MemberKind kind) const {
  if (kind == reflection::MemberKind::EnumMember &&
      (type.constraint.nominal == nullptr || !type.constraint.nominal->IsEnum() || type.is_void)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto position = TypePosition(type);
  const auto nominal = type.constraint.nominal != nullptr || type.constraint.interface != nullptr;
  if (nominal && (!position || (image_->reflection_types.values()[*position].flags &
                                reflection::kRegistered) == 0)) {
    return std::unexpected(ExecutionStatus::ReflectionUnavailable);
  }
  return position;
}

std::span<const std::uint32_t> ReflectionIndex::Entries(
    std::uint32_t type,
    reflection::MemberScope scope) const noexcept {
  const auto& view = views_.values()[type];
  return scope == reflection::MemberScope::Declared || view.inherited == nullptr
             ? view.declared
             : std::span(view.inherited, view.size);
}

std::uint64_t ReflectionIndex::MemberKey(const NativeReflectionMember& member) const {
  return MemberLookupKey(member.kind, member.name.text(), member.type_arguments.values(),
                         [this](auto index) { return TypeAt(index); });
}

std::expected<const NativeReflectionMember*, ExecutionStatus> ReflectionIndex::FindMember(
    ReflectedType type,
    reflection::MemberKind kind,
    std::string_view name,
    reflection::MemberQuery query,
    std::span<const ReflectedType> arguments) const {
  const auto position = QueryType(type, kind);
  if (!position) {
    return std::unexpected(position.error());
  }
  if (!*position) {
    return nullptr;
  }
  const auto& view = views_.values()[**position];
  const auto hash = MemberLookupKey(static_cast<std::uint32_t>(kind), name, arguments,
                                    [](const auto& argument) { return argument; });
  const auto matches =
      std::ranges::equal_range(std::span(view.lookup, view.size), hash, {}, &Lookup::key);
  const auto members = image_->reflection_members.values();
  const auto declared_owner = image_->reflection_types.values()[**position].type;
  for (const auto& entry : matches) {
    const auto& member = members[entry.index];
    if (member.name.text() == name && MatchesArguments(member.type_arguments.values(), arguments) &&
        (query.scope != reflection::MemberScope::Declared ||
         member.declaring_type == declared_owner) &&
        Matches(member, kind, query)) {
      return &member;
    }
  }
  return nullptr;
}

std::expected<void, ExecutionStatus> ReflectionIndex::EnumerateMembers(
    ReflectedType type,
    reflection::MemberKind kind,
    reflection::MemberQuery query,
    absl::FunctionRef<void(const NativeReflectionMember&)> visit) const {
  const auto position = QueryType(type, kind);
  if (!position) {
    return std::unexpected(position.error());
  }
  if (*position) {
    const auto entries = Entries(**position, kind == reflection::MemberKind::EnumMember
                                                 ? reflection::MemberScope::IncludeInherited
                                                 : query.scope);
    const auto members = image_->reflection_members.values();
    for (const auto index : entries) {
      if (Matches(members[index], kind, query)) {
        visit(members[index]);
      }
    }
  }
  return {};
}

std::expected<void, ExecutionStatus> ReflectionIndex::PrepareView(std::uint32_t index) {
  auto& view = views_.values()[index];
  const auto& type = image_->reflection_types.values()[index];
  const auto parent = TypePosition(type.base);
  const auto inherited = parent ? Entries(*parent, reflection::MemberScope::IncludeInherited)
                                : std::span<const std::uint32_t>{};
  const auto capacity = view.declared.size() + inherited.size();
  if (capacity == 0) {
    view.prepared = true;
    return {};
  }
  const auto reflected_type = TypeAt(type.type);
  const auto enumeration =
      reflected_type.constraint.nominal != nullptr && reflected_type.constraint.nominal->IsEnum();
  if (inherited.empty() && !enumeration) {
    view.size = view.declared.size();
  } else {
    view.inherited =
        static_cast<std::uint32_t*>(heap_->AllocateNative(capacity * sizeof(std::uint32_t)));
    if (view.inherited == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    MergeInheritedMembers(view, inherited);
  }
  const auto members = image_->reflection_members.values();
  const auto output = Entries(index, reflection::MemberScope::IncludeInherited);
  if (view.size != 0) {
    view.lookup = static_cast<Lookup*>(heap_->AllocateNative(view.size * sizeof(Lookup)));
    if (view.lookup == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
  }
  const auto lookup = std::span(view.lookup, view.size);
  for (std::size_t item = 0; item < view.size; ++item) {
    lookup[item] = {.key = MemberKey(members[output[item]]), .index = output[item]};
  }
  std::ranges::sort(lookup, {}, &Lookup::key);
  if (enumeration) {
    std::ranges::sort(std::span(view.inherited, view.size), {},
                      [&](auto member) { return members[member].target; });
  }
  view.prepared = true;
  return {};
}

void ReflectionIndex::MergeInheritedMembers(View& view, std::span<const std::uint32_t> inherited) {
  const auto members = image_->reflection_members.values();
  const auto output = std::span(view.inherited, view.declared.size() + inherited.size());
  std::size_t own = 0;
  std::size_t base = 0;
  while (own < view.declared.size() || base < inherited.size()) {
    const auto own_name =
        own < view.declared.size() ? members[view.declared[own]].name.text() : std::string_view{};
    const auto base_name =
        base < inherited.size() ? members[inherited[base]].name.text() : std::string_view{};
    const auto use_own =
        own < view.declared.size() && (base == inherited.size() || own_name <= base_name);
    if (use_own) {
      while (own < view.declared.size() && members[view.declared[own]].name.text() == own_name) {
        output[view.size++] = view.declared[own++];
      }
      while (base < inherited.size() && members[inherited[base]].name.text() == own_name) {
        ++base;
      }
    } else {
      const auto member = inherited[base++];
      if (members[member].kind != static_cast<std::uint32_t>(reflection::MemberKind::Constructor)) {
        output[view.size++] = member;
      }
    }
  }
}

std::expected<ReflectionIndex*, ExecutionStatus> PrepareReflectionIndex(NativeInstance& instance) {
  auto& index = instance.program().reflection;
  if (index == nullptr) {
    auto prepared = ReflectionIndex::Create(instance.isolate().heap(), instance.image(),
                                            instance.layouts().types);
    if (!prepared) {
      return std::unexpected(prepared.error());
    }
    index = std::move(*prepared);
  }
  return index.get();
}

std::expected<void, ExecutionStatus> ReflectionIndex::PrepareAttributes() {
  const auto attributes = image_->attributes.values();
  for (const auto& attribute : attributes) {
    if (AttributedMethod(attribute) == nullptr) {
      continue;
    }
    const auto* nominal = TypeAt(attribute.type).constraint.nominal;
    while (nominal != nullptr) {
      ++attribute_count_;
      nominal = nominal->base();
    }
  }
  if (attribute_count_ == 0) {
    return {};
  }
  attribute_lookup_ =
      static_cast<Lookup*>(heap_->AllocateNative(attribute_count_ * sizeof(Lookup)));
  if (attribute_lookup_ == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto entries = std::span(attribute_lookup_, attribute_count_);
  std::size_t output = 0;
  for (std::size_t index = 0; index < attributes.size(); ++index) {
    if (AttributedMethod(attributes[index]) == nullptr) {
      continue;
    }
    const auto* nominal = TypeAt(attributes[index].type).constraint.nominal;
    while (nominal != nullptr) {
      entries[output++] = {
          .key = absl::HashOf(nominal),
          .index = static_cast<std::uint32_t>(index),
      };
      nominal = nominal->base();
    }
  }
  std::ranges::sort(entries, [](const auto& left, const auto& right) {
    return std::pair(left.key, left.index) < std::pair(right.key, right.index);
  });
  return {};
}

const NativeReflectionMember* ReflectionIndex::AttributedMethod(
    const NativeAttribute& attribute) const {
  const auto* member = Member(attribute.target);
  return member != nullptr && Matches(*member, reflection::MemberKind::Method, {}) ? member
                                                                                   : nullptr;
}

void ReflectionIndex::EnumerateMethodAttributes(
    ReflectedType type,
    std::optional<std::string_view> module,
    absl::FunctionRef<void(const NativeAttribute&, const NativeReflectionMember&)> visit) const {
  const auto entries = std::span(attribute_lookup_, attribute_count_);
  const auto* nominal = type.constraint.nominal;
  const auto candidates =
      std::ranges::equal_range(entries, absl::HashOf(nominal), {}, &Lookup::key);
  for (const auto& entry : candidates) {
    const auto& attribute = image_->attributes.values()[entry.index];
    if (!TypeAt(attribute.type).constraint.nominal->IsSubtypeOf(*nominal)) {
      continue;
    }
    const auto* member = Member(attribute.target);
    if (module) {
      const auto* owner = Type(TypeAt(member->declaring_type));
      if (owner->module.text() != *module) {
        continue;
      }
    }
    visit(attribute, *member);
  }
}

std::expected<void, ExecutionStatus> ReflectionIndex::Prepare() {
  const auto types = image_->reflection_types.values();
  for (std::size_t index = 0; index < types.size(); ++index) {
    const auto row = static_cast<std::uint32_t>(index);
    const auto& type = types[index];
    type_positions_.values()[index] = {.key = type.type, .index = row};
    type_constraints_.values()[index] = {.key = TypeHash(TypeAt(type.type)), .index = row};
    type_names_.values()[index] = {
        .key = TypeLookupKey(type.module.text(), type.name.text(), type.arguments.values(),
                             [this](auto argument) { return TypeAt(argument); }),
        .index = row,
    };
  }
  std::ranges::sort(type_positions_.values(), {}, &Lookup::key);
  std::ranges::sort(type_constraints_.values(), {}, &Lookup::key);
  std::ranges::sort(type_names_.values(), {}, &Lookup::key);
  const auto members = image_->reflection_members.values();
  for (std::size_t index = 0; index < members.size(); ++index) {
    const auto row = static_cast<std::uint32_t>(index);
    declared_.values()[index] = row;
    member_ids_.values()[index] = {.key = members[index].declaration.value, .index = row};
  }
  std::ranges::sort(member_ids_.values(), {}, &Lookup::key);
  std::ranges::sort(declared_.values(), {},
                    [&](auto index) { return std::pair(members[index].declaring_type, index); });
  for (std::size_t begin = 0; begin < members.size();) {
    const auto owner = members[declared_.values()[begin]].declaring_type;
    auto end = begin + 1;
    while (end < members.size() && members[declared_.values()[end]].declaring_type == owner) {
      ++end;
    }
    views_.values()[*TypePosition(owner)].declared = declared_.values().subspan(begin, end - begin);
    begin = end;
  }
  NativeBuffer<std::uint32_t> pending(*heap_, types.size());
  if (!pending.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  for (std::size_t root = 0; root < types.size(); ++root) {
    std::size_t count = 0;
    auto next = std::optional(static_cast<std::uint32_t>(root));
    while (next && !views_.values()[*next].prepared) {
      pending.values()[count++] = *next;
      next = TypePosition(types[*next].base);
    }
    while (count != 0) {
      const auto prepared = PrepareView(pending.values()[--count]);
      if (!prepared) {
        return std::unexpected(prepared.error());
      }
    }
  }
  return PrepareAttributes();
}

}  // namespace vela::runtime
