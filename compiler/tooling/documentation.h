#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/token.h"

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
// 无文件上下文的语法文档生成，供编辑器与内存源码调用。
[[nodiscard]] std::expected<std::string, frontend::Diagnostics> GenerateModuleDocumentation(
    std::string_view source);

}  // namespace vela::tooling
