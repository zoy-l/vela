#pragma once

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

// 在控制流收窄前确定可被闭包写入的声明；结果只借用该 AST 的稳定节点地址。
void CollectCapturedMutableDeclarations(const Program& program,
                                        absl::flat_hash_set<const void*>& declarations);
void CollectCapturedMutableDeclarations(const Function& function,
                                        absl::flat_hash_set<const void*>& declarations);
void CollectCapturedMutableDeclarations(const Class& owner,
                                        absl::flat_hash_set<const void*>& declarations);

}  // namespace vela::frontend
