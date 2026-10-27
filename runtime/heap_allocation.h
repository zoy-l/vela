#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/object.h"

namespace vela::runtime {

// 初始化不得分配或失败；对象的引用及有效长度全部就绪后才发布给 GC。
template <class Initialize>
HeapObject* Heap::AllocateObject(ObjectKind kind,
                                 std::size_t size,
                                 const Initialize& initialize) noexcept {
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = kind;
  initialize(*object);
  Publish(*object, allocation);
  return object;
}

template <class T>
HeapObject* Heap::AllocateNativeObject(ObjectKind kind, T payload) noexcept {
  auto owned = std::unique_ptr<T>(new (std::nothrow) T(std::move(payload)));
  if (owned == nullptr) {
    return nullptr;
  }
  return AllocateObject(kind, sizeof(HeapObject), [&owned](HeapObject& object) {
    if constexpr (std::is_same_v<T, Future>) {
      owned->MakeHeapOwned();
    }
    object.native_payload = owned.release();
  });
}

}  // namespace vela::runtime
