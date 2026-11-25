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

#include "compiler/development/development_files.h"

namespace vela::compiler {

// 本地捕获独占文件图、解析缓存和 OS 订阅；进程外实现只拥有 IPC 会话。
class LocalDevelopmentFiles final : public DevelopmentFiles {
 public:
  explicit LocalDevelopmentFiles(std::string entry, frontend::ModulePaths imports = {});
  [[nodiscard]] std::expected<SourceUpdate, frontend::Diagnostics> Capture(
      const CompiledSourceSnapshot& previous) override;
  [[nodiscard]] std::expected<void, frontend::Diagnostics> StartWatching(
      std::function<void()> changed = {}) override;
  [[nodiscard]] std::expected<void, frontend::Diagnostics> WatchSources(
      std::span<const SourceChange> changes) override;
  [[nodiscard]] std::expected<void, frontend::Diagnostics> Accept(
      std::shared_ptr<const CompiledSourceSnapshot> snapshot) override;
  [[nodiscard]] std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> Poll(
      std::chrono::milliseconds timeout = {}) override;
  [[nodiscard]] std::string entry() const override;

 private:
  enum class GraphTracking : std::uint8_t { Existing, Expanded };
  struct FullCapture final {
    SourceUpdate update;
    bool notify = false;
  };
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
  std::expected<FullCapture, frontend::Diagnostics> CaptureAll(
      const CompiledSourceSnapshot& previous);
  std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> Rescan(
      const CompiledSourceSnapshot& previous);
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
  bool rescan_incomplete_ = false;
};

}  // namespace vela::compiler
