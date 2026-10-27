#pragma once

#include <optional>
#include <string_view>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

inline constexpr std::string_view kStandardSourcePrefix = "builtin:";
inline constexpr std::string_view kErrorSourceName = "builtin:Error";

[[nodiscard]] bool IsStandardName(std::string_view name) noexcept;
[[nodiscard]] std::optional<std::string_view> StandardDeclarationSource(
    std::string_view name) noexcept;
// 声明按源码标识符需要加入；用户模块无需导入，也不能覆盖这些全局名称。
[[nodiscard]] Diagnostics AddStandardEnvironment(Program& program);
[[nodiscard]] bool IsStandardTypeNamespace(const Program& program, const Class& declaration);

}  // namespace vela::frontend
