#pragma once

#include <absl/container/flat_hash_map.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/module_binding.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_file.h"

namespace vela::frontend {

struct SourceInput final {
  std::string_view path;
  std::string_view text;
};

// 编辑器消费生产 parser 保留的完整声明，同时保留诊断；编译入口始终拒绝语法错误。
enum class SourceLoadMode : std::uint8_t { Compilation, Editor };

// 会话拥有的已接纳模块快照；调用方在编译成功后才提交工作副本。
struct ModuleCache final {
  absl::flat_hash_map<std::string, std::shared_ptr<const ParsedSource>> sources;
  absl::flat_hash_map<std::string, std::string> resolutions;
  absl::flat_hash_map<std::string, std::uint32_t> binding_ids;
};

// 解析阶段拥有未绑定的模块树；消费后转移给绑定阶段，不复制节点。
struct ModuleGraph final {
  std::vector<SourceModule> modules;
  std::vector<SourceFile> sources;
  std::vector<FileResolution> file_resolutions;
  Diagnostics diagnostics;
  Diagnostics syntax_diagnostics;
};

[[nodiscard]] ModuleGraph LoadModuleGraph(std::string_view path);
[[nodiscard]] ModuleGraph LoadModuleGraph(std::span<const std::string> roots,
                                          ModuleCache& cache,
                                          std::span<const SourceInput> overlays);
[[nodiscard]] ParseResult BindModuleGraph(
    ModuleGraph graph,
    std::span<const std::string> retained_standard_names = {});

// 无扩展名的导入直接映射到 .vela 文件，不探测无后缀文件或目录入口。
[[nodiscard]] std::filesystem::path ResolveImportPath(const std::filesystem::path& importer,
                                                      std::filesystem::path imported);

[[nodiscard]] ParseResult LoadPreparedProgram(std::string_view path,
                                              ParsedSource source,
                                              ModuleCache& cache);

// 路径使用 UTF-8；覆盖内容只在加载期间借用，可用于编辑器未保存的模块。
[[nodiscard]] ParseResult LoadProgram(std::string_view path,
                                      std::span<const SourceInput> overlays = {},
                                      SourceLoadMode mode = SourceLoadMode::Compilation);
// 一批根模块共享不可变解析快照；编辑变化由新的 cache 批次隔离。
[[nodiscard]] ParseResult LoadProgram(std::string_view path,
                                      ModuleCache& cache,
                                      std::span<const SourceInput> overlays,
                                      SourceLoadMode mode = SourceLoadMode::Compilation);
// 内存模块只从给定输入解析依赖，不隐式读取磁盘；entry 的模块编号为零。
[[nodiscard]] ParseResult LoadSources(std::span<const SourceInput> sources, std::string_view entry);
[[nodiscard]] ParseResult LoadSources(std::span<const SourceInput> sources,
                                      std::span<const std::string> roots,
                                      std::span<const std::string> retained_standard_names = {});
[[nodiscard]] ParseResult LoadSources(std::span<const SourceInput> sources,
                                      std::span<const std::string> roots,
                                      ModuleCache& cache,
                                      std::span<const std::string> retained_standard_names = {});

}  // namespace vela::frontend
