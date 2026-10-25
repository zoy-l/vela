#pragma once

#include <absl/container/flat_hash_map.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/parser.h"

namespace vela::frontend {

struct SourceInput final {
  std::string_view path;
  std::string_view text;
};

// 会话拥有的已接纳模块快照；调用方在编译成功后才提交工作副本。
struct ModuleCache final {
  absl::flat_hash_map<std::string, std::shared_ptr<const std::string>> sources;
  absl::flat_hash_map<std::string, std::string> resolutions;
  absl::flat_hash_map<std::string, std::uint32_t> binding_ids;
};

[[nodiscard]] auto LoadPreparedProgram(std::string_view path,
                                       Program root,
                                       std::string source,
                                       ModuleCache& cache) -> ParseResult;

// 路径使用 UTF-8；覆盖内容只在加载期间借用，可用于编辑器未保存的模块。
[[nodiscard]] auto LoadProgram(std::string_view path, std::span<const SourceInput> overlays = {})
    -> ParseResult;
// 内存模块只从给定输入解析依赖，不隐式读取磁盘；entry 的模块编号为零。
[[nodiscard]] auto LoadSources(std::span<const SourceInput> sources, std::string_view entry)
    -> ParseResult;

}  // namespace vela::frontend
