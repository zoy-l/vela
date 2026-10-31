#pragma once

#include <span>
#include <string_view>

#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/checker.h"

namespace vela::frontend {

// 测试输入仍经过生产模块加载、标准环境、特化和检查路径。
inline CheckResult CheckTestSource(std::string_view source) {
  const SourceInput input{.path = "main.vela", .text = source};
  return CheckParsed(LoadSources(std::span(&input, 1), input.path));
}

inline CheckResult CheckTestModules(std::span<const SourceInput> sources, std::string_view entry) {
  return CheckParsed(LoadSources(sources, entry));
}

}  // namespace vela::frontend
