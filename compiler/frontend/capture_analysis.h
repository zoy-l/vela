#pragma once

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

// 在控制流收窄前确定可被闭包写入的声明；结果只借用该 AST 的稳定节点地址。
[[nodiscard]] absl::flat_hash_set<const void*> FindCapturedMutableDeclarations(
    const Program& program);

}  // namespace vela::frontend
