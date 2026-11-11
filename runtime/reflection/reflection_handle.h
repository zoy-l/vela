#pragma once

#include <cstddef>
#include <expected>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/reflection/reflection_index.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class NativeDispatch;

// 脚本句柄只通过普通 GC 边保留封闭状态和结构类型；会话不持有历史发布。
struct ReflectionHandle final {
  ReflectedType type;
  DeclarationId member;
  Value anchor;
  NativeDispatch* dispatch = nullptr;
};

[[nodiscard]] const ReflectionHandle* AsReflectionHandle(Value value) noexcept;
[[nodiscard]] std::expected<HeapObject*, ExecutionStatus> ReflectionProgram(
    const ReflectionHandle& handle);
[[nodiscard]] bool IsEqualReflectionIdentity(const HeapObject& left,
                                             const HeapObject& right) noexcept;
[[nodiscard]] std::size_t HashReflectionIdentity(const HeapObject& object) noexcept;

}  // namespace vela::runtime
