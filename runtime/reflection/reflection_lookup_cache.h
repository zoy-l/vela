#pragma once

#include <cstdint>

#include "base/reflection_operation.h"
#include "runtime/value/value.h"

namespace vela::runtime {

struct NativeProgramState;

[[nodiscard]] constexpr bool IsCachedMemberLookup(reflection::Operation operation) noexcept {
  using reflection::Operation;
  return operation == Operation::FindField || operation == Operation::FindProperty ||
         operation == Operation::FindMethod || operation == Operation::FindStaticField ||
         operation == Operation::FindStaticProperty || operation == Operation::FindStaticMethod;
}

// 单槽缓存只记住默认查询；键和结果由拥有它的 GC 对象追踪。运行时
// ExecuteMetadataLookup 与 compiler/native/reflection_emitter.cc 的快路径必须共同比较
// epoch、program、operation、result_type、name 和 qualifier，字段顺序由 NativeMemoryLayout
// 与 offsetof 共同固定。
// 实例和发布共同守卫类型编号，不能跨程序复用泛型描述符包装。
struct ReflectedLookupCache final {
  const std::uint64_t* epoch = nullptr;
  std::uint64_t observed = 0;
  const NativeProgramState* program = nullptr;
  Value qualifier;
  Value name;
  Value result;
  std::uint32_t result_type = 0;
  std::uint32_t operation = 0;

  template <class Mark>
  void TraceValues(const Mark& mark) const {
    mark(qualifier);
    mark(name);
    mark(result);
  }
};

}  // namespace vela::runtime
