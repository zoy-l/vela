#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/directory_watcher.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::compiler {

// 文件监视器的解析缓存只按内容复用不可变语法，不保存已提交身份或运行时地址。
// Capture 固定本次全部模块文本；Poll 只捕获 OS 事件指向的文件及新依赖。
// 后续编译不得再次读取磁盘；未接纳的编辑保持 dirty，直到提交或被后续编辑替换。
// 显式 reset 的捕获与编译线程的轮询共享同一事务锁。
class DevelopmentFiles final {
 public:
  explicit DevelopmentFiles(std::string entry);
  [[nodiscard]] std::expected<SourceUpdate, frontend::Diagnostics> Capture(
      const CompiledSourceSnapshot& previous);
  [[nodiscard]] std::expected<void, frontend::Diagnostics> StartWatching();
  // 显式加载其他入口时登记其文件；后续事件不需要遍历整个已提交源码表。
  [[nodiscard]] std::expected<void, frontend::Diagnostics> WatchSources(
      std::span<const SourceChange> changes);
  // 所有者在每次源码提交后传入该快照；普通提交只消费 changed_sources，Reload 重建订阅。
  // 接纳较早候选不会覆盖其后已捕获的未提交文本。
  [[nodiscard]] std::expected<void, frontend::Diagnostics> Accept(
      std::shared_ptr<const CompiledSourceSnapshot> snapshot);
  [[nodiscard]] std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> Poll(
      std::chrono::milliseconds timeout = {});
  // 首次成功 Capture 后是规范化的模块 URI，创建源码会话时使用同一身份。
  [[nodiscard]] std::string entry() const;

 private:
  enum class GraphTracking : std::uint8_t { Existing, Expanded };
  std::expected<void, frontend::Diagnostics> Track(const std::string& path);
  std::expected<void, frontend::Diagnostics> TrackReferences();
  std::expected<void, frontend::Diagnostics> WatchDirectories(
      const std::string& path,
      std::span<const std::filesystem::path> directories);
  std::expected<void, frontend::Diagnostics> WatchDirectory(const std::string& path,
                                                            const std::filesystem::path& directory,
                                                            DirectoryWatchScope scope);
  std::expected<void, frontend::Diagnostics> Reference(const std::string& path);
  void Release(const std::string& path);
  void Detach(const std::string& path);
  std::expected<void, frontend::Diagnostics> WatchGroup(const std::string& root,
                                                        absl::flat_hash_set<std::string> paths);
  void ReleaseGroupPath(std::string_view root, const std::string& path);
  void RetireWatchGroup(const std::string& root);
  void RetainSyntax(const CompiledSourceSnapshot& snapshot, const std::string& path);
  frontend::Diagnostics AdoptSource(const CompiledSourceSnapshot& snapshot,
                                    const std::string& path);
  std::expected<void, frontend::Diagnostics> RefreshDirectory(
      const std::filesystem::path& native,
      absl::flat_hash_set<std::string>& changed);
  std::expected<GraphTracking, frontend::Diagnostics> TrackGraph(
      const std::string& root,
      const frontend::CapturedSources& graph);
  std::expected<frontend::CapturedSources, frontend::Diagnostics> ReadWatchedGraph(
      const std::string& root,
      std::span<const frontend::SourceInput> inputs);
  std::expected<frontend::CapturedSources, frontend::Diagnostics> ReadGraph(
      const CompiledSourceSnapshot& previous);
  std::expected<SourceUpdate, frontend::Diagnostics> CaptureAll(
      const CompiledSourceSnapshot& previous);
  std::expected<void, frontend::Diagnostics> ReadReflectionConfiguration();
  [[nodiscard]] bool HasPending() const;
  std::optional<std::string_view> CapturedText(const CompiledSourceSnapshot& previous,
                                               const std::string& path) const;
  std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> CaptureChanges(
      const CompiledSourceSnapshot& previous,
      std::span<const std::string> paths);
  std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> CaptureEvents(
      const CompiledSourceSnapshot& previous,
      const FileChanges& events);
  SourceUpdate Pending(const CompiledSourceSnapshot& previous);
  void SetPending(std::string path, std::optional<std::string> text);
  void ErasePending(const std::string& path);
  void PrunePending(const CompiledSourceSnapshot& previous);
  std::string entry_;
  mutable std::mutex mutex_;
  frontend::ModuleCache cache_;
  std::unique_ptr<DirectoryWatcher> watcher_;
  std::shared_ptr<const CompiledSourceSnapshot> accepted_;
  absl::flat_hash_map<std::string, std::size_t> references_;
  absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>> accepted_sources_;
  absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>> watch_groups_;
  absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>> group_users_;
  absl::flat_hash_map<std::string, std::vector<std::filesystem::path>> subscriptions_;
  using PathUsers = absl::flat_hash_map<std::filesystem::path,
                                        absl::flat_hash_set<std::string>,
                                        std::hash<std::filesystem::path>>;
  PathUsers directory_sources_;
  absl::flat_hash_map<std::filesystem::path, std::string, std::hash<std::filesystem::path>>
      native_paths_;
  // dirty 条目密集存储；哈希表只负责定位，稀疏容量不会增加每次编辑的遍历成本。
  std::vector<SourceChange> pending_;
  absl::flat_hash_map<std::string, std::size_t> pending_indices_;
  absl::flat_hash_map<std::string, std::string> aliases_;
  bool rebase_pending_ = false;
  std::vector<frontend::ReflectionSelection> reflection_;
  bool reflection_changed_ = false;
  absl::flat_hash_set<std::string> configuration_paths_;
  bool configuration_invalid_ = false;
};

}  // namespace vela::compiler
