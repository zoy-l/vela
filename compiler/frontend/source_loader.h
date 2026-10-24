#pragma once

#include <string_view>

#include "compiler/frontend/parser.h"

namespace vela::frontend {

// 路径使用 UTF-8；按依赖顺序解析每个文件一次，并诊断导入环及 I/O、语法错误。
[[nodiscard]] auto LoadProgram(std::string_view path) -> ParseResult;

}  // namespace vela::frontend
