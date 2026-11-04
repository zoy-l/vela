#pragma once

#include <cstdint>
#include <memory>

#include "runtime/heap/host_memory.h"

namespace vela::runtime {

class Heap;

enum class HostObjectError : std::uint8_t {
  OutOfMemory,
  OwnershipConflict,
  InvalidSharedOwner,
  ForeignIsolate,
};

// 位于宿主包装头之后；普通借用只读 HeapObject::native_payload，不复制这里的所有权。
class HostObjectStorage final {
 public:
  using Destructor = void (*)(void*) noexcept;
  HostObjectStorage(void* pointer, Destructor destroy, HostMemory memory = {}) noexcept;
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

}  // namespace vela::runtime
