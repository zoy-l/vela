#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "compiler/frontend/diagnostic.h"

namespace vela::tooling {

// 对完整模块图做静态检查，再从入口原文生成导出 API 的 Markdown；不执行脚本。
[[nodiscard]] auto GenerateDocumentation(std::string_view entry)
    -> std::expected<std::string, frontend::Diagnostics>;
// 无文件上下文的语法文档生成，供编辑器与内存源码调用。
[[nodiscard]] auto GenerateModuleDocumentation(std::string_view source)
    -> std::expected<std::string, frontend::Diagnostics>;

}  // namespace vela::tooling
