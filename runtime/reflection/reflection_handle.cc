#include "runtime/reflection/reflection_handle.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <absl/hash/hash.h>

#include "base/execution_status.h"
#include "base/reflection.h"
#include "runtime/heap/field_storage.h"
#include "runtime/heap/heap.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

namespace {

template <class Cache>
void BindEpoch(const NativeInstance& instance, Cache& cache) noexcept {
  if (const auto* session = instance.dispatch()) {
    cache.epoch = &session->publication_epoch();
    cache.observed = *cache.epoch;
  } else {
    cache.epoch = &cache.observed;
  }
}

}  // namespace

std::expected<const NativeReflectionMember*, ExecutionStatus> ReflectionHandle::ResolveMember(
    ReflectionIndex& index,
    std::uint64_t revision) const {
  if (cached_member != nullptr && cached_revision == revision) {
    return cached_member;
  }
  const auto resolved = index.ResolveMember(member);
  if (resolved) {
    cached_member = *resolved;
    cached_revision = revision;
  }
  return resolved;
}

void ReflectionHandle::CacheField(const NativeInstance& instance,
                                  const NativeReflectionMember& resolved,
                                  Value receiver) const noexcept {
  if (resolved.kind != static_cast<std::uint32_t>(reflection::MemberKind::Field) ||
      (resolved.flags & reflection::kStatic) != 0) {
    return;
  }
  const auto& constraint = instance.layouts().types[resolved.type];
  // 名义对象和容器仍需完整契约检查；enum 还需要盒装，不能直接复制存储表示。
  if (!constraint.dynamic && constraint.kind == ValueKind::Object) {
    return;
  }
  const auto& layout = *Heap::ClassOf(*receiver.AsObject());
  auto& cache = field_cache;
  cache.reloadable = layout.storage() == ClassStorage::Reloadable;
  if (cache.reloadable) {
    const auto slot = layout.FieldIndex(resolved.declaration);
    if (!slot) {
      std::unreachable();
    }
    cache.slot = *slot;
  } else {
    cache.slot = resolved.target;
  }
  cache.kind_offset = FieldStorage::KindOffset(layout.field_count(), cache.slot);
  cache.kind = constraint.kind;
  cache.nullable = constraint.nullable;
  cache.dynamic = constraint.dynamic;
  cache.layout_id = layout.id();
  BindEpoch(instance, cache);
}

void ReflectionHandle::CacheCall(const NativeInstance& instance,
                                 const NativeReflectionMember& resolved,
                                 Value receiver,
                                 std::uint32_t target) const noexcept {
  const auto parameters = resolved.parameters.values();
  const auto& image = instance.image();
  const auto& function = image.function(target);
  const auto& program = instance.program();
  const auto* layout = receiver.IsObject() ? Heap::ClassOf(*receiver.AsObject()) : nullptr;
  if ((function.flags & kNativeEnumResult) != 0 || (!receiver.IsNull() && layout == nullptr)) {
    return;
  }
  if ((function.flags & kNativeReflectionReceiverLayout) != 0) {
    const auto receiver_class = instance.image().types.values()[function.receiver_type].index;
    const auto direct_layouts = std::span(program.direct_field_layouts, image.classes.size);
    if (layout != direct_layouts[receiver_class]) {
      return;
    }
  }
  const auto& types = instance.layouts().types;
  for (const auto& parameter : parameters) {
    const auto& constraint = types[parameter.type];
    if (!constraint.dynamic && constraint.kind == ValueKind::Object) {
      return;
    }
  }
  auto& cache = call_cache;
  cache.layout_id = layout != nullptr ? layout->id() : 0;
  cache.program = &program;
  cache.entry = function.reflection_entry;
  cache.count = static_cast<std::uint32_t>(parameters.size());
  BindEpoch(instance, cache);
}

HeapObject* Heap::AllocateReflectionHandle(const ReflectionHandle& handle) noexcept {
  const RootRange state_root(*this, &handle.anchor, 1, std::nullopt);
  const RootRange type_root(*this, std::span(&handle.type.constraint, 1));
  const auto size = sizeof(HeapObject) + sizeof(ReflectionHandle);
  return AllocateObject(ObjectKind::Reflection, size, [&](HeapObject& object) {
    std::construct_at(StorageAfter<ReflectionHandle>(object), handle);
  });
}

const ReflectionHandle* AsReflectionHandle(Value value) noexcept {
  return value.IsObject() && Heap::KindOf(*value.AsObject()) == ObjectKind::Reflection
             ? StorageAfter<ReflectionHandle>(*value.AsObject())
             : nullptr;
}

std::expected<HeapObject*, ExecutionStatus> ReflectionProgram(const ReflectionHandle& handle) {
  if (const auto* dispatch = handle.dispatch) {
    return dispatch->BorrowCurrentState();
  }
  return handle.anchor.AsObject();
}

bool IsEqualReflectionIdentity(const HeapObject& left, const HeapObject& right) noexcept {
  const auto* right_layout = Heap::ClassOf(right);
  if (right_layout == nullptr || !right_layout->HasReflectionIdentity()) {
    return false;
  }
  const auto* first = AsReflectionHandle(Heap::LoadField(left, 0));
  const auto* second = AsReflectionHandle(Heap::LoadField(right, 0));
  const auto same_context =
      first->member == DeclarationId{} ||
      (first->dispatch == second->dispatch &&
       (first->dispatch != nullptr || first->anchor.AsObject() == second->anchor.AsObject()));
  return same_context && first->type == second->type && first->member == second->member;
}

std::size_t HashReflectionIdentity(const HeapObject& object) noexcept {
  const auto* handle = AsReflectionHandle(Heap::LoadField(object, 0));
  const auto member = handle->member != DeclarationId{};
  return absl::HashOf(handle->type, handle->member, member ? handle->dispatch : nullptr,
                      member && !handle->anchor.IsNull() ? handle->anchor.AsObject() : nullptr);
}

}  // namespace vela::runtime
