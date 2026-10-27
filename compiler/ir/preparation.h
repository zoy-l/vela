#pragma once

#include <expected>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"
#include "compiler/ir/persistent_ids.h"

namespace vela::ir {

// 语义模型借用 AST 节点；离线编译产物在后端消费完成前同时拥有二者。
// 此边界不创建运行时对象，也不执行 VM 寄存器分配或原生符号绑定。
struct PreparedProgram final {
  frontend::Program source;
  frontend::SemanticModel model;
  Program program;
};

[[nodiscard]] std::expected<PreparedProgram, frontend::Diagnostics> PrepareParsed(
    frontend::ParseResult parsed);

// REPL 已完成会话级检查，沿用相同 lowering/优化并保留事务中的稳定编号。
[[nodiscard]] LoweringResult PrepareProgram(const frontend::Program& source,
                                            const frontend::SemanticModel& model,
                                            PersistentIds* ids = nullptr);

}  // namespace vela::ir
