#include "runtime/reflection/reflection_handle.h"

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>

#include <absl/hash/hash.h>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

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
