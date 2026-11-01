#pragma once

#include <span>
#include <string>
#include <string_view>

#include "base/source_location.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::frontend {
struct Function;
}

namespace vela::tooling {

[[nodiscard]] std::string FunctionSignature(const frontend::Function& function);
[[nodiscard]] std::string DocumentationComment(std::string_view source,
                                               std::span<const frontend::SourceComment> comments,
                                               SourceLocation location);

struct DocumentationResult final {
  std::string markdown;
  frontend::Diagnostics diagnostics;
  [[nodiscard]] bool Succeeded() const { return !frontend::HasErrors(diagnostics); }
};

// 从入口未绑定语法树生成文档，完整模块图静态检查通过后交付；不执行脚本。
[[nodiscard]] DocumentationResult GenerateDocumentation(std::string_view entry,
                                                        frontend::DiagnosticOptions options = {});

}  // namespace vela::tooling
