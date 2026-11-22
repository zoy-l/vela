#pragma once

#include <cstdint>
#include <memory>

#include "runtime/heap/host_memory.h"

namespace vela::runtime {

class Heap;
struct HeapObject;

enum class HostObjectError : std::uint8_t {
  OutOfMemory,
  OwnershipConflict,
  InvalidSharedOwner,
  ForeignIsolate,
};

// 新构造的宿主值先拥有未发布的非移动堆块；构造或发布失败直接归还，不暴露半成品给 GC。
class HostConstruction final {
 public:
  ~HostConstruction();
  HostConstruction(const HostConstruction&) = delete;
  HostConstruction& operator=(const HostConstruction&) = delete;
  HostConstruction(HostConstruction&& other) noexcept;
  HostConstruction& operator=(HostConstruction&&) = delete;
  [[nodiscard]] void* pointer() const noexcept;

 private:
  friend class Heap;
  HostConstruction(Heap& heap, HeapObject& object) noexcept;
  [[nodiscard]] bool IsOwnedBy(const Heap& heap) const noexcept { return heap_ == &heap; }
  Heap* heap_;
  HeapObject* object_;
};

// 位于宿主包装头之后；普通借用只读 HeapObject::native_payload，不复制这里的所有权。
class HostObjectStorage final {
 public:
  using Destructor = void (*)(void*) noexcept;
  HostObjectStorage(void* pointer, Destructor destroy) noexcept;
  explicit HostObjectStorage(std::shared_ptr<void> shared) noexcept;
  ~HostObjectStorage();
  HostObjectStorage(const HostObjectStorage&) = delete;
  HostObjectStorage& operator=(const HostObjectStorage&) = delete;
  HostObjectStorage(HostObjectStorage&& other) noexcept;
  HostObjectStorage& operator=(HostObjectStorage&&) = delete;

  [[nodiscard]] void* pointer() const noexcept { return pointer_; }

 private:
  friend class Heap;
  // 声明在最前，使实际 C++ 对象及共享引用释放后再退还预算。
  HostMemory memory_;
  void* pointer_ = nullptr;
  Destructor destroy_ = nullptr;
  std::shared_ptr<void> shared_;
};

// 内联值没有外部 unique/shared owner；额外内存预算和析构器仍随包装存活。
struct ConstructedHostStorage final {
  HostMemory memory;
  HostObjectStorage::Destructor destroy;
};

}  // namespace vela::runtime
