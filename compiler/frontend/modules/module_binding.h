#pragma once

#include <cstdint>
#include <vector>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_file.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"

namespace vela::frontend {

// 文件图解析完成后绑定；dependencies 与该模块的 import 指令逐项对应。
struct SourceModule final {
  Program program;
  std::vector<std::uint32_t> dependencies;
  std::uint32_t binding_id = 0;
};

[[nodiscard]] ParseResult BindModules(std::vector<SourceModule> modules,
                                      std::vector<SourceFile> sources);

// 标准声明不参与用户模块的名字保留检查，但复用相同的类型参数绑定。
[[nodiscard]] Diagnostics BindStandardDeclarations(Program& program, SourceFile source);

}  // namespace vela::frontend
