#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <llvm/IR/ConstantRange.h>

#include "compiler/ir/ir.h"

namespace vela::compiler::native {

inline constexpr auto kUnknownArrayElementBits = std::numeric_limits<std::uint64_t>::max();

// 数组事实与其元素反馈得到的整数证明共用同一规范 IR；发射器不能按改写后编号索引。
// element_bits、fixed_storage 按规范寄存器索引，其余字段按规范指令位置索引。
struct ArrayEmissionFacts final {
  std::span<const std::uint64_t> element_bits;
  std::span<const std::uint8_t> fixed_storage;
  std::span<const std::uint8_t> no_overflow;
  std::span<const std::uint8_t> in_bounds;
  std::span<const std::uint8_t> reserved_pushes;
};

// 索引空间与 ArrayEmissionFacts 相同；此类型拥有该函数的事实存储。
struct ArrayAccessFacts final {
  std::vector<std::uint64_t> element_bits;
  std::vector<std::uint8_t> fixed_storage;
  std::vector<std::uint8_t> no_overflow;
  std::vector<std::uint8_t> in_bounds;
  std::vector<std::uint8_t> reserved_pushes;

  [[nodiscard]] ArrayEmissionFacts emission() const {
    return {
        .element_bits = element_bits,
        .fixed_storage = fixed_storage,
        .no_overflow = no_overflow,
        .in_bounds = in_bounds,
        .reserved_pushes = reserved_pushes,
    };
  }
};

struct ClosedArrayAccessFacts final {
  std::vector<std::uint8_t> roots;
  std::vector<std::uint8_t> reachable;
  std::vector<std::vector<llvm::ConstantRange>> parameters;
  std::vector<ArrayAccessFacts> functions;
};

// 调用方统一选定模式；只有封闭链接程序可请求固定存储或跨函数范围证明。
enum class ArrayAnalysisMode : std::uint8_t { Elements, FixedStorage, Ranges };

struct ProgramArrayAccessFacts final {
  std::vector<ArrayAccessFacts> generic;
  ClosedArrayAccessFacts closed;
};

// 在规范 IR 上合并非逃逸 int[] 的全部写入；封闭程序还合并经全局槽共享的别名。
// 按函数、寄存器返回可能置位的位掩码，全一表示未知。参数、未知定义和外部访问使整个
// 别名组失效；可重载程序只证明函数内数组，不能假设其他代码版本的全局写入受约束。
// 数组复制通过读取依赖传播到不动点；任一来源的未知写入会传递到所有派生数组。
// 通用入口与直接调用域分别持有别名关系；同一域的元素写入、固定存储和长度共享扫描。
[[nodiscard]] ProgramArrayAccessFacts AnalyzeArrayAccess(const ir::Program& program,
                                                         ArrayAnalysisMode mode);

}  // namespace vela::compiler::native
