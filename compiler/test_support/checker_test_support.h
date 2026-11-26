#pragma once

#include <span>
#include <string_view>

#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/typing/checker.h"

namespace vela::frontend {

// 测试输入仍经过生产模块加载、标准环境、特化和检查路径。
inline ParseResult ParseTestSource(std::string_view source) {
  const SourceInput input{.path = "main.vela", .text = source};
  return LoadSources(std::span(&input, 1), input.path);
}

inline CheckResult CheckTestSource(std::string_view source) {
  return CheckParsed(ParseTestSource(source));
}

inline CheckResult CheckTestModules(std::span<const SourceInput> sources, std::string_view entry) {
  return CheckParsed(LoadSources(sources, entry));
}

}  // namespace vela::frontend
