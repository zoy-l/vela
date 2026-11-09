#pragma once

#include <cstdint>

#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/lowerer.h"

namespace vela::ir {

struct PreparationOptions final {
  frontend::DeclarationIds* declarations = nullptr;
  std::uint64_t revision = 0;
  NativeProgramMode native_mode = NativeProgramMode::Closed;
};

// 返回的 IR 拥有全部布局描述和源码路径，不借用 AST、语义模型或运行时状态。
// 原生代码生成与符号绑定由后续阶段执行。
[[nodiscard]] LoweringResult PrepareParsed(frontend::ParseResult parsed,
                                           frontend::DiagnosticOptions options = {});

// REPL 已完成会话级检查，沿用相同 lowering/优化及声明身份绑定。
[[nodiscard]] LoweringResult PrepareProgram(const frontend::Program& source,
                                            const frontend::SemanticModel& model,
                                            const PreparationOptions& options = {});

}  // namespace vela::ir
