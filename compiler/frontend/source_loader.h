#pragma once

#include <absl/container/flat_hash_map.h>
#include <cstdint>
#include <filesystem>
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

// 编辑器消费生产 parser 保留的完整声明，同时保留诊断；编译入口始终拒绝语法错误。
enum class SourceLoadMode : std::uint8_t { Compilation, Editor };

// 会话拥有的已接纳模块快照；调用方在编译成功后才提交工作副本。
struct ModuleCache final {
  absl::flat_hash_map<std::string, std::shared_ptr<const std::string>> sources;
  absl::flat_hash_map<std::string, std::string> resolutions;
  absl::flat_hash_map<std::string, std::uint32_t> binding_ids;
};

// 无扩展名的导入直接映射到 .vela 文件，不探测无后缀文件或目录入口。
[[nodiscard]] std::filesystem::path ResolveImportPath(const std::filesystem::path& importer,
                                                      std::filesystem::path imported);

[[nodiscard]] ParseResult LoadPreparedProgram(std::string_view path,
                                              Program root,
                                              std::string source,
                                              ModuleCache& cache);

// 路径使用 UTF-8；覆盖内容只在加载期间借用，可用于编辑器未保存的模块。
[[nodiscard]] ParseResult LoadProgram(std::string_view path,
                                      std::span<const SourceInput> overlays = {},
                                      SourceLoadMode mode = SourceLoadMode::Compilation);
// 内存模块只从给定输入解析依赖，不隐式读取磁盘；entry 的模块编号为零。
[[nodiscard]] ParseResult LoadSources(std::span<const SourceInput> sources, std::string_view entry);

}  // namespace vela::frontend
