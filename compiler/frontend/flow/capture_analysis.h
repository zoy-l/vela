#pragma once

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

struct CapturedDeclarations final {
  // 可变捕获共享存储；实际被赋值的捕获可能在调用或挂起期间变化。
  absl::flat_hash_set<const void*> mutables;
  absl::flat_hash_set<const void*> assigned;
};

// 在控制流收窄前分析捕获；结果只借用该 AST 的稳定节点地址。
void CollectCapturedDeclarations(const Program& program, CapturedDeclarations& declarations);
void CollectCapturedDeclarations(const Function& function, CapturedDeclarations& declarations);
void CollectCapturedDeclarations(const Class& owner, CapturedDeclarations& declarations);

}  // namespace vela::frontend
