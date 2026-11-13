#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/native/native_image.h"
#include "runtime/reflection/reflection_index.h"
#include "runtime/reflection/reflection_lookup_cache.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class NativeDispatch;
class NativeInstance;
struct NativeProgramState;

// 入口只借用当前版本的代码；先验证 epoch 与执行实例，再解引用入口。
struct ReflectedCallCache final {
  const std::uint64_t* epoch = nullptr;
  std::uint64_t observed = 0;
  std::uint64_t layout_id = 0;
  const NativeProgramState* program = nullptr;
  NativeReflectionEntry entry = nullptr;
  std::uint32_t count = 0;
};

// 发布 epoch 与物理布局共同守卫原生字段访问；不保留接收者、类型图或历史映像。
struct ReflectedFieldCache final {
  const std::uint64_t* epoch = nullptr;
  std::uint64_t observed = 0;
  std::uint64_t layout_id = 0;
  std::uint64_t kind_offset = 0;
  std::uint32_t slot = 0;
  ValueKind kind = ValueKind::Null;
  bool nullable = false;
  bool dynamic = false;
  bool reloadable = false;
};

// 脚本句柄只通过普通 GC 边保留封闭状态和结构类型；会话不持有历史发布。
struct ReflectionHandle final {
  ReflectedType type;
  DeclarationId member;
  Value anchor;
  NativeDispatch* dispatch = nullptr;
  // 仅借用一次发布的成员；先比较版本再访问指针，不保活历史映像。
  mutable const NativeReflectionMember* cached_member = nullptr;
  mutable std::uint64_t cached_revision = 0;
  mutable ReflectedFieldCache field_cache;
  mutable ReflectedCallCache call_cache;
  mutable ReflectedLookupCache lookup_cache;

  [[nodiscard]] std::expected<const NativeReflectionMember*, ExecutionStatus> ResolveMember(
      ReflectionIndex& index,
      std::uint64_t revision) const;
  void CacheField(const NativeInstance& instance,
                  const NativeReflectionMember& resolved,
                  Value receiver) const noexcept;
  void CacheCall(const NativeInstance& instance,
                 const NativeReflectionMember& resolved,
                 Value receiver,
                 std::uint32_t target) const noexcept;
};

[[nodiscard]] const ReflectionHandle* AsReflectionHandle(Value value) noexcept;
[[nodiscard]] std::expected<HeapObject*, ExecutionStatus> ReflectionProgram(
    const ReflectionHandle& handle);
[[nodiscard]] bool IsEqualReflectionIdentity(const HeapObject& left,
                                             const HeapObject& right) noexcept;
[[nodiscard]] std::size_t HashReflectionIdentity(const HeapObject& object) noexcept;

}  // namespace vela::runtime
