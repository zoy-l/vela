#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "compiler/frontend/diagnostic.h"

namespace vela::tooling {

// 从入口未绑定语法树生成文档，完整模块图静态检查通过后交付；不执行脚本。
[[nodiscard]] std::expected<std::string, frontend::Diagnostics> GenerateDocumentation(
    std::string_view entry);
// 无文件上下文的语法文档生成，供编辑器与内存源码调用。
[[nodiscard]] std::expected<std::string, frontend::Diagnostics> GenerateModuleDocumentation(
    std::string_view source);

}  // namespace vela::tooling
