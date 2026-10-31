#pragma once

#include <span>
#include <string_view>

#include "compiler/frontend/source_loader.h"
#include "compiler/ir/preparation.h"

namespace vela::ir::test {

// 所有后端测试经由同一生产加载和准备路径，返回结果拥有后续编译所需的数据。
inline LoweringResult Prepare(std::string_view source) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
}

}  // namespace vela::ir::test
