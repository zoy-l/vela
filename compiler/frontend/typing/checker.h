#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"

namespace vela::frontend {

struct CheckResult final {
  Diagnostics diagnostics;
  std::size_t function_count = 0;

  [[nodiscard]] bool Succeeded() const { return !HasErrors(diagnostics); }
};

[[nodiscard]] CheckResult CheckParsed(ParseResult parsed, DiagnosticOptions options = {});
// 已绑定模块的 AST 经过泛型特化后保留，供文档等工具继续读取同一次加载的源码快照。
[[nodiscard]] CheckResult CheckProgram(Program& program, DiagnosticOptions options = {});
[[nodiscard]] CheckResult CheckFile(std::string_view path,
                                    std::span<const SourceInput> overlays = {},
                                    DiagnosticOptions options = {});

}  // namespace vela::frontend
