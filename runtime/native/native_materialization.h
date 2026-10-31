#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/native/native_image.h"
#include "runtime/value/value.h"

namespace vela::runtime {

struct NativeCheckpoint;

struct NativeVirtualField final {
  DeclarationId declaration;
  std::uint32_t value = 0;
  std::uint32_t object = kNoNativeIndex;
  FieldState state = FieldState::Ready;
};

struct NativeVirtualObject final {
  // 编号由 checkpoint 的旧版本程序状态解释，名义身份随该状态保活。
  std::uint32_t class_index = 0;
  std::uint32_t storage = 0;
  std::uint32_t root = 0;
  NativeTable<NativeVirtualField> fields;
};

struct NativeVirtualAlias final {
  std::uint32_t reg = 0;
  std::uint32_t object = 0;
};

// 描述由函数版本拥有；仅使用稳定身份、快照槽和图内编号，不包含 LLVM 对象。
struct NativeVirtualGraph final {
  NativeTable<NativeVirtualObject> objects;
  NativeTable<NativeVirtualAlias> aliases;
};

// 准备期间独占临时映射并精确保根。所有帧和迁移均准备成功后才 Commit；
// 拒绝事务时析构只释放临时根，半成品留给正常 GC，不修改脚本别名或原生局部槽。
class NativeMaterialization final {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<NativeMaterialization>, ExecutionStatus>
  Prepare(Heap& heap, NativeCheckpoint& checkpoint) noexcept;
  [[nodiscard]] static bool HasVirtualObjects(const NativeCheckpoint& checkpoint) noexcept;
  ~NativeMaterialization();
  NativeMaterialization(const NativeMaterialization&) = delete;
  NativeMaterialization& operator=(const NativeMaterialization&) = delete;
  NativeMaterialization(NativeMaterialization&&) = delete;
  NativeMaterialization& operator=(NativeMaterialization&&) = delete;
  void Commit() noexcept;
  [[nodiscard]] std::size_t object_count() const noexcept { return object_count_; }

 private:
  struct Work final {
    std::uint32_t node = 0;
    bool queued = false;
  };
  NativeMaterialization(Heap& heap, NativeCheckpoint& checkpoint) noexcept;
  [[nodiscard]] ExecutionStatus PrepareStorage() noexcept;
  [[nodiscard]] ExecutionStatus BuildGraph() noexcept;
  [[nodiscard]] ExecutionStatus AllocateGraph() noexcept;
  void Enqueue(std::uint32_t node) noexcept;
  Heap* heap_;
  NativeCheckpoint* checkpoint_;
  std::span<Value> objects_;
  std::span<Work> work_;
  RootRange roots_{RootOwner::Runtime};
  std::size_t queued_ = 0;
  std::size_t object_count_ = 0;
};

}  // namespace vela::runtime
