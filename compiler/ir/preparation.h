#pragma once

#include <cstdint>

#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/lowerer.h"
#include "compiler/ir/persistent_ids.h"

namespace vela::ir {

struct PreparationOptions final {
  PersistentIds* persistent_slots = nullptr;
  frontend::DeclarationIds* declarations = nullptr;
  std::uint64_t revision = 0;
  NativeProgramMode native_mode = NativeProgramMode::Closed;
};

// 返回的 IR 拥有全部布局描述和源码路径，不借用 AST、语义模型或运行时状态。
// 此边界不执行 VM 寄存器分配或原生符号绑定。
[[nodiscard]] LoweringResult PrepareParsed(frontend::ParseResult parsed,
                                           frontend::DiagnosticOptions options = {});

// REPL 已完成会话级检查，沿用相同 lowering/优化并保留事务中的稳定编号。
[[nodiscard]] LoweringResult PrepareProgram(const frontend::Program& source,
                                            const frontend::SemanticModel& model,
                                            const PreparationOptions& options = {});

}  // namespace vela::ir
