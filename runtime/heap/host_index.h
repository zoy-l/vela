#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include "base/execution_status.h"

namespace vela::runtime {

class Heap;
class NominalType;
struct HeapObject;

// 弱索引不保根。GC 在释放宿主所有权前移除条目；扩容可能 GC，只能在输入已保根时准备。
class HostIndex final {
 public:
  explicit HostIndex(Heap& heap) noexcept : heap_(&heap) {}
  ~HostIndex();
  HostIndex(const HostIndex&) = delete;
  HostIndex& operator=(const HostIndex&) = delete;
  HostIndex(HostIndex&&) = delete;
  HostIndex& operator=(HostIndex&&) = delete;

  [[nodiscard]] HeapObject* Find(const NominalType& type, const void* pointer) noexcept;
  [[nodiscard]] std::expected<void, ExecutionStatus> PrepareInsert() noexcept;
  void Insert(const NominalType& type, const void* pointer, HeapObject& object) noexcept;
  void Erase(const NominalType& type, const void* pointer) noexcept;
  void ReleaseEmpty() noexcept;

 private:
  enum class State : std::uint8_t { Empty, Live, Removed };
  struct Entry final {
    const NominalType* type = nullptr;
    const void* pointer = nullptr;
    HeapObject* object = nullptr;
    State state = State::Empty;
  };
  [[nodiscard]] Entry* FindEntry(const NominalType& type, const void* pointer) noexcept;
  static State InsertEntry(Entry* entries, std::size_t capacity, Entry value) noexcept;

  Heap* heap_;
  Entry* entries_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t size_ = 0;
  std::size_t removed_ = 0;
};

}  // namespace vela::runtime
