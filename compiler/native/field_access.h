#pragma once

#include <absl/container/flat_hash_map.h>

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

}  // namespace vela::compiler::native
