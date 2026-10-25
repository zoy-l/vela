#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"

namespace vela::frontend {

struct CheckResult final {
  Diagnostics diagnostics;
  std::size_t function_count = 0;

  [[nodiscard]] auto Succeeded() const -> bool { return diagnostics.empty(); }
};

[[nodiscard]] auto CheckSource(std::string_view source) -> CheckResult;
// 已绑定模块的 AST 经过泛型特化后保留，供文档等工具继续读取同一次加载的源码快照。
[[nodiscard]] auto CheckProgram(Program& program) -> CheckResult;
[[nodiscard]] auto CheckFile(std::string_view path, std::span<const SourceInput> overlays = {})
    -> CheckResult;
[[nodiscard]] auto CheckModules(std::span<const SourceInput> sources, std::string_view entry)
    -> CheckResult;

}  // namespace vela::frontend
