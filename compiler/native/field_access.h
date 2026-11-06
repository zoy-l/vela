#pragma once

#include <absl/container/flat_hash_map.h>

#include <cstdint>
#include <span>
#include <vector>

#include "base/declaration.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

struct FieldTarget final {
  ir::ClassId owner;
  ir::FieldId slot;
};

using FieldTargets = absl::flat_hash_map<DeclarationId, FieldTarget>;

// 字段身份在 dynamic 擦除后仍保留；代码生成和缓存指纹共用同一个布局选择。
[[nodiscard]] FieldTargets FindFieldTargets(const ir::Program& program);

enum class FieldKindWrite : std::uint8_t { Required, Redundant };

// 只复用同一基本块内已初始化的标量标签；别名写入、寄存器改写和安全点使事实失效。
[[nodiscard]] std::vector<FieldKindWrite> FindFieldKindWrites(
    const ir::Function& function,
    const ir::Program& program,
    std::span<const std::uint8_t> safepoints);

}  // namespace vela::compiler::native
