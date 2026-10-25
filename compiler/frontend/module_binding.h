#pragma once

#include <cstdint>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_file.h"

namespace vela::frontend {

// 文件图解析完成后绑定；dependencies 与该模块的 import 指令逐项对应。
struct SourceModule final {
  Program program;
  std::vector<std::uint32_t> dependencies;
  std::uint32_t binding_id = 0;
};

[[nodiscard]] auto BindModules(std::vector<SourceModule> modules, std::vector<SourceFile> sources)
    -> ParseResult;

}  // namespace vela::frontend
