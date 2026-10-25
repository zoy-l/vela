#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "compiler/frontend/diagnostic.h"

namespace vela::tooling {

// 只重排语法间空白；注释、字面量及插值原文保留。非法源码返回解析诊断。
[[nodiscard]] auto FormatSource(std::string_view source)
    -> std::expected<std::string, frontend::Diagnostics>;

}  // namespace vela::tooling
