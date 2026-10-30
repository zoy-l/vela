#pragma once

#include <expected>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::tooling {

// 输入为已绑定且通过生产 checker 的 API 模块；只读取声明，不执行脚本。
[[nodiscard]] std::expected<std::string, frontend::Diagnostics> GenerateMocks(
    const frontend::Program& program,
    std::span<const std::string> interfaces,
    const std::filesystem::path& output);

int MockCommand(std::span<char*> arguments, std::ostream& output, std::ostream& errors);

}  // namespace vela::tooling
