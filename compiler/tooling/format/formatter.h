#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "compiler/frontend/diagnostic.h"

namespace vela::tooling {

// 统一空白与单引号风格，保留字符串值和注释；非法源码返回解析诊断。
[[nodiscard]] std::expected<std::string, frontend::Diagnostics> FormatSource(
    std::string_view source);

}  // namespace vela::tooling
