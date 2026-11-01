#include "compiler/development/development_files.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/directory_watcher.h"
#include "base/file.h"
#include "base/path.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/builtins/sdk_modules.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_file.h"
#include "compiler/frontend/modules/source_loader.h"

namespace vela::compiler {
namespace {

frontend::Diagnostics FileError(const std::string& path, std::string message) {
  return {{.location = {}, .message = std::move(message), .source_path = path}};
}

bool IsDiskSource(const CompiledSourceSnapshot& source, const std::string& path) {
  return source.sources.contains(path) && !source.prepared_sources.contains(path) &&
         !path.starts_with(frontend::kSdkSourcePrefix);
}

std::vector<std::string> ChangedPaths(const CompiledSourceSnapshot& previous,
                                      const CompiledSourceSnapshot& next) {
  if (previous.session == next.session) {
    return next.changed_sources;
  }
  absl::flat_hash_set<std::string> paths;
  for (const auto& [path, text] : previous.sources) {
    static_cast<void>(text);
    paths.insert(path);
  }
  for (const auto& [path, text] : next.sources) {
    static_cast<void>(text);
    paths.insert(path);
  }
  return {paths.begin(), paths.end()};
}

std::expected<absl::flat_hash_map<std::string, std::string>, frontend::Diagnostics> ReadKnownFiles(
    const absl::flat_hash_set<std::string>& paths,
    frontend::ModuleCache& cache) {
  absl::flat_hash_map<std::string, std::string> texts;
  for (const auto& path : paths) {
    std::error_code error;
    const auto file = Utf8Path(path);
    const auto exists = std::filesystem::exists(file, error);
    if (error) {
      return std::unexpected(FileError(path, error.message()));
    }
    if (!exists) {
      cache.sources.erase(path);
      continue;
    }
    const auto bytes = ReadBinaryFile(file);
    if (!bytes) {
      return std::unexpected(FileError(path, bytes.error()));
    }
    const void* storage = bytes->data();
    texts.emplace(path, std::string(static_cast<const char*>(storage), bytes->size()));
  }
  return texts;
}

}  // namespace

DevelopmentFiles::DevelopmentFiles(std::string entry)
    : entry_(std::move(entry)), accepted_(std::make_shared<const CompiledSourceSnapshot>()) {}

std::string DevelopmentFiles::entry() const {
  const std::scoped_lock lock(mutex_);
  return entry_;
}

std::expected<void, frontend::Diagnostics> DevelopmentFiles::Track(const std::string& path) {
  if (!watcher_) {
    return {};
  }
  const auto file = Utf8Path(path);
  const auto [subscription, inserted] = subscriptions_.try_emplace(path);
  auto& directories = subscription->second;
  if (inserted) {
    const auto entry_directory = Utf8Path(entry_).parent_path();
    const auto parent_directory = file.parent_path();
    const auto inside = std::ranges::mismatch(entry_directory, parent_directory);
    const auto anchor = inside.in1 == entry_directory.end() ? entry_directory : parent_directory;
    const auto ancestor = anchor.parent_path();
    for (auto directory = file.parent_path(); !directory.empty();) {
      directories.push_back(directory);
      directory_sources_.try_emplace(directory).first->second.insert(path);
      const auto parent = directory.parent_path();
      if (directory == ancestor || parent == directory) {
        break;
      }
      directory = parent;
    }
    std::ranges::reverse(directories);
  }
  native_paths_.insert_or_assign(file, path);
  // 祖先事件通过反向索引定位实际受影响文件，同时覆盖已存在目录改名和缺失目录重建。
  // 先登记入口树的父目录，避免在建立子目录监听期间漏掉目录身份变化。
#ifdef _WIN32
  const auto& tree = directories.at(directories.size() > 1 ? 1 : 0);
#endif
  for (const auto& directory : directories) {
#ifdef _WIN32
    if (directory != directories.front() && directory != tree) {
      continue;
    }
#endif
    std::error_code error;
    const auto exists = std::filesystem::exists(directory, error);
    if (error) {
      return std::unexpected(FileError(path, error.message()));
    }
    if (!exists) {
      watcher_->Unwatch(directory);
      continue;
    }
#ifdef _WIN32
    const auto watched =
        watcher_->Watch(directory, directory == tree ? DirectoryWatchScope::Descendants
                                                     : DirectoryWatchScope::Direct);
#else
    const auto watched = watcher_->Watch(directory);
#endif
    if (!watched) {
      return std::unexpected(FileError(path, watched.error()));
    }
  }
  return {};
}

void DevelopmentFiles::Detach(const std::string& path) {
  const auto found = subscriptions_.find(path);
  if (found != subscriptions_.end()) {
    for (const auto& directory : found->second) {
      auto users = directory_sources_.find(directory);
      users->second.erase(path);
      if (users->second.empty()) {
        watcher_->Unwatch(directory);
        directory_sources_.erase(users);
      }
    }
    subscriptions_.erase(found);
  }
}

std::expected<void, frontend::Diagnostics> DevelopmentFiles::Reference(const std::string& path) {
  if (path.starts_with(frontend::kSdkSourcePrefix)) {
    return {};
  }
  ++references_.try_emplace(path, 0).first->second;
  return Track(path);
}

void DevelopmentFiles::Release(const std::string& path) {
  const auto found = references_.find(path);
  if (found == references_.end() || --found->second != 0) {
    return;
  }
  Detach(path);
  references_.erase(found);
  native_paths_.erase(Utf8Path(path));
  aliases_.erase(path);
  cache_.sources.erase(path);
  ErasePending(path);
}

std::expected<void, frontend::Diagnostics> DevelopmentFiles::WatchGroup(
    const std::string& root,
    absl::flat_hash_set<std::string> paths) {
  auto& previous = watch_groups_.try_emplace(root).first->second;
  for (const auto& path : paths) {
    const auto watched = previous.contains(path) ? Track(path) : Reference(path);
    previous.insert(path);
    group_users_.try_emplace(path).first->second.insert(root);
    if (!watched) {
      // 已取得的引用仍属于该组；下一次编辑可重试失败的订阅。
      return watched;
    }
  }
  for (const auto& path : previous) {
    if (!paths.contains(path)) {
      ReleaseGroupPath(root, path);
    }
  }
  previous = std::move(paths);
  return {};
}

void DevelopmentFiles::ReleaseGroupPath(std::string_view root, const std::string& path) {
  const auto users = group_users_.find(path);
  users->second.erase(root);
  if (users->second.empty()) {
    group_users_.erase(users);
  }
  Release(path);
}

void DevelopmentFiles::RetireWatchGroup(const std::string& root) {
  const auto found = watch_groups_.find(root);
  if (found == watch_groups_.end()) {
    return;
  }
  for (const auto& path : found->second) {
    ReleaseGroupPath(root, path);
  }
  watch_groups_.erase(found);
}

void DevelopmentFiles::RetainSyntax(const CompiledSourceSnapshot& snapshot,
                                    const std::string& path) {
  const auto pending = pending_indices_.find(path);
  if (pending != pending_indices_.end() &&
      pending_.at(pending->second).text != *snapshot.sources.at(path)) {
    return;
  }
  const auto syntax = snapshot.modules.sources.find(path);
  if (syntax != snapshot.modules.sources.end()) {
    cache_.sources.insert_or_assign(path, syntax->second);
  }
}

frontend::Diagnostics DevelopmentFiles::AdoptSource(const CompiledSourceSnapshot& snapshot,
                                                    const std::string& path) {
  absl::flat_hash_set<std::string> paths;
  if (IsDiskSource(snapshot, path)) {
    paths.insert(path);
    RetainSyntax(snapshot, path);
  }
  const auto syntax = snapshot.modules.sources.find(path);
  if (snapshot.sources.contains(path) && syntax != snapshot.modules.sources.end()) {
    for (const auto& imported : syntax->second->program.imports) {
      if (!imported.path.starts_with(frontend::kSdkSourcePrefix)) {
        paths.insert(
            PathText(frontend::ResolveImportPath(Utf8Path(path), Utf8Path(imported.path))));
      }
    }
  }
  auto& previous = accepted_sources_.try_emplace(path).first->second;
  frontend::Diagnostics diagnostics;
  for (const auto& source : paths) {
    if (previous.contains(source)) {
      continue;
    }
    const auto watched = Reference(source);
    if (!watched) {
      diagnostics.insert(diagnostics.end(), watched.error().begin(), watched.error().end());
    }
  }
  for (const auto& source : previous) {
    if (!paths.contains(source)) {
      Release(source);
    }
  }
  if (paths.empty()) {
    accepted_sources_.erase(path);
  } else {
    previous = std::move(paths);
  }
  return diagnostics;
}

std::expected<void, frontend::Diagnostics> DevelopmentFiles::StartWatching() {
  const std::scoped_lock lock(mutex_);
  watcher_ = std::make_unique<DirectoryWatcher>();
  std::error_code error;
  const auto entry = std::filesystem::weakly_canonical(Utf8Path(entry_), error);
  if (error) {
    return std::unexpected(FileError(entry_, error.message()));
  }
  entry_ = PathText(entry);
  const auto watched = WatchGroup(entry_, {entry_});
  if (!watched) {
    return watched;
  }
  for (const auto& [path, count] : references_) {
    static_cast<void>(count);
    const auto registered = Track(path);
    if (!registered) {
      return registered;
    }
  }
  return {};
}

std::expected<void, frontend::Diagnostics> DevelopmentFiles::WatchSources(
    std::span<const SourceChange> changes) {
  const std::scoped_lock lock(mutex_);
  for (const auto& change : changes) {
    if (change.text) {
      SetPending(change.path, change.text);
      const auto tracked = WatchGroup(change.path, {change.path});
      if (!tracked) {
        return tracked;
      }
    }
  }
  return {};
}

std::expected<void, frontend::Diagnostics> DevelopmentFiles::Accept(
    std::shared_ptr<const CompiledSourceSnapshot> snapshot) {
  const std::scoped_lock lock(mutex_);
  if (snapshot == accepted_) {
    return {};
  }
  const auto paths = ChangedPaths(*accepted_, *snapshot);
  frontend::Diagnostics diagnostics;
  for (const auto& path : paths) {
    const auto errors = AdoptSource(*snapshot, path);
    diagnostics.insert(diagnostics.end(), errors.begin(), errors.end());
  }
  accepted_ = std::move(snapshot);
  PrunePending(*accepted_);
  // 新基线必须重新发送尚未接纳的文本；不能依赖用户再保存一次来触发事件。
  rebase_pending_ = !pending_.empty();
  if (!diagnostics.empty()) {
    return std::unexpected(std::move(diagnostics));
  }
  return {};
}

std::expected<frontend::CapturedSources, frontend::Diagnostics> DevelopmentFiles::ReadGraph(
    const CompiledSourceSnapshot& previous) {
  absl::flat_hash_set<std::string> paths{entry_};
  for (const auto& [path, source] : cache_.sources) {
    paths.insert(path);
  }
  for (const auto& [path, text] : previous.sources) {
    if (!previous.prepared_sources.contains(path)) {
      paths.insert(path);
    }
  }
  auto read = ReadKnownFiles(paths, cache_);
  if (!read) {
    return std::unexpected(std::move(read.error()));
  }
  const auto& texts = *read;
  std::vector<frontend::SourceInput> inputs;
  for (const auto& [path, text] : texts) {
    inputs.push_back({.path = path, .text = text});
  }
  std::vector<std::string> roots{entry_};
  for (const auto& [path, text] : previous.sources) {
    if (path != entry_ && texts.contains(path) && !previous.prepared_sources.contains(path)) {
      roots.push_back(path);
    }
  }
  // 文件解析缓存不固定路径解析；symlink 等文件身份变化在本次捕获中重新确认。
  cache_.resolutions.clear();
  return frontend::CaptureSourceFiles(roots, cache_, inputs);
}

std::expected<DevelopmentFiles::GraphTracking, frontend::Diagnostics> DevelopmentFiles::TrackGraph(
    const std::string& root,
    const frontend::CapturedSources& graph) {
  absl::flat_hash_set<std::string> paths{root};
  for (const auto& source : graph.sources) {
    paths.insert(source.path);
    const auto syntax = cache_.sources.find(source.path);
    if (syntax == cache_.sources.end()) {
      continue;
    }
    for (const auto& imported : syntax->second->program.imports) {
      if (!imported.path.starts_with(frontend::kSdkSourcePrefix)) {
        paths.insert(
            PathText(frontend::ResolveImportPath(Utf8Path(source.path), Utf8Path(imported.path))));
      }
    }
  }
  for (const auto& resolution : graph.file_resolutions) {
    if (resolution.requested_path != resolution.canonical_path) {
      aliases_.insert_or_assign(resolution.requested_path, resolution.canonical_path);
      paths.insert(resolution.requested_path);
    }
  }
  const auto expanded =
      std::ranges::any_of(paths, [&](const auto& path) { return !references_.contains(path); });
  const auto watched = WatchGroup(root, std::move(paths));
  if (!watched) {
    return std::unexpected(watched.error());
  }
  return expanded ? GraphTracking::Expanded : GraphTracking::Existing;
}

std::expected<frontend::CapturedSources, frontend::Diagnostics> DevelopmentFiles::ReadWatchedGraph(
    const std::string& root,
    std::span<const frontend::SourceInput> inputs) {
  absl::flat_hash_map<std::string, std::string> texts;
  std::vector<frontend::SourceInput> captured(inputs.begin(), inputs.end());
  while (true) {
    auto graph =
        frontend::CaptureSourceFiles(std::span(&root, 1), cache_, captured, &accepted_->modules);
    const auto tracked = TrackGraph(root, graph);
    if (!tracked) {
      return std::unexpected(tracked.error());
    }
    if (!watcher_ || *tracked == GraphTracking::Existing) {
      return graph;
    }
    // 新发现的依赖先订阅再读取，关闭初次解析与订阅之间的文件写入窗口。
    absl::flat_hash_set<std::string> paths{root};
    for (const auto& source : graph.sources) {
      paths.insert(source.path);
    }
    auto read = ReadKnownFiles(paths, cache_);
    if (!read) {
      return std::unexpected(std::move(read.error()));
    }
    texts = std::move(*read);
    captured.clear();
    for (const auto& [path, text] : texts) {
      captured.push_back({.path = path, .text = text});
    }
  }
}

std::expected<SourceUpdate, frontend::Diagnostics> DevelopmentFiles::Capture(
    const CompiledSourceSnapshot& previous) {
  const std::scoped_lock lock(mutex_);
  return CaptureAll(previous);
}

std::expected<SourceUpdate, frontend::Diagnostics> DevelopmentFiles::CaptureAll(
    const CompiledSourceSnapshot& previous) {
  for (const auto& [path, count] : references_) {
    static_cast<void>(count);
    const auto tracked = Track(path);
    if (!tracked) {
      return std::unexpected(tracked.error());
    }
  }
  auto loaded = ReadGraph(previous);
  if (!loaded) {
    return std::unexpected(std::move(loaded.error()));
  }
  auto& graph = *loaded;
  const auto tracked = TrackGraph(entry_, graph);
  if (!tracked) {
    return std::unexpected(tracked.error());
  }
  if (!graph.diagnostics.empty()) {
    return std::unexpected(std::move(graph.diagnostics));
  }
  entry_ = graph.sources.front().path;
  if (watcher_ && *tracked == GraphTracking::Expanded) {
    // 新目录已开始报告事件，再次捕获关闭“先读取、后登记”之间的窗口。
    return CaptureAll(previous);
  }
  SourceUpdate update;
  update.base_revision = previous.revision;
  absl::flat_hash_set<std::string> captured;
  const absl::flat_hash_set<std::string> accepted_roots(previous.roots.begin(),
                                                        previous.roots.end());
  for (const auto& source : graph.sources) {
    captured.insert(source.path);
    const auto old = previous.sources.find(source.path);
    if (old == previous.sources.end() || *old->second != source.text) {
      update.changes.push_back({.path = source.path, .text = std::string(source.text)});
    }
    if (!accepted_roots.contains(source.path)) {
      update.added_roots.push_back(source.path);
    }
  }
  for (const auto& [path, text] : previous.sources) {
    if (!captured.contains(path) && !previous.prepared_sources.contains(path)) {
      update.changes.push_back({.path = path, .text = std::nullopt});
    }
  }
  std::ranges::sort(update.changes, {}, &SourceChange::path);
  std::ranges::sort(update.added_roots);
  pending_.clear();
  pending_indices_.clear();
  for (const auto& change : update.changes) {
    SetPending(change.path, change.text);
  }
  return update;
}

void DevelopmentFiles::SetPending(std::string path, std::optional<std::string> text) {
  const auto [found, inserted] = pending_indices_.try_emplace(path, pending_.size());
  if (inserted) {
    pending_.push_back({.path = std::move(path), .text = std::move(text)});
  } else {
    pending_.at(found->second).text = std::move(text);
  }
}

void DevelopmentFiles::ErasePending(const std::string& path) {
  const auto found = pending_indices_.find(path);
  if (found == pending_indices_.end()) {
    return;
  }
  const auto index = found->second;
  pending_indices_.erase(found);
  if (index + 1 != pending_.size()) {
    auto& change = pending_.at(index);
    change = std::move(pending_.back());
    pending_indices_.at(change.path) = index;
  }
  pending_.pop_back();
}

void DevelopmentFiles::PrunePending(const CompiledSourceSnapshot& previous) {
  std::vector<std::string> retired;
  std::size_t index = 0;
  while (index < pending_.size()) {
    auto& change = pending_.at(index);
    const auto& text = change.text;
    const auto old = previous.sources.find(change.path);
    const auto same = old == previous.sources.end() ? !text : text && *old->second == *text;
    if (same) {
      const auto path = change.path;
      retired.push_back(path);
      ErasePending(path);
      continue;
    }
    ++index;
  }
  if (!pending_indices_.contains(entry_)) {
    retired.push_back(entry_);
  }
  for (const auto& path : retired) {
    RetireWatchGroup(path);
  }
}

SourceUpdate DevelopmentFiles::Pending(const CompiledSourceSnapshot& previous) {
  PrunePending(previous);
  SourceUpdate update;
  update.base_revision = previous.revision;
  update.changes = pending_;
  for (const auto& change : pending_) {
    if (change.text && !previous.sources.contains(change.path)) {
      update.added_roots.push_back(change.path);
    }
  }
  std::ranges::sort(update.changes, {}, &SourceChange::path);
  std::ranges::sort(update.added_roots);
  return update;
}

std::optional<std::string_view> DevelopmentFiles::CapturedText(
    const CompiledSourceSnapshot& previous,
    const std::string& path) const {
  const auto pending = pending_indices_.find(path);
  if (pending != pending_indices_.end()) {
    const auto& text = pending_.at(pending->second).text;
    return text ? std::optional<std::string_view>(*text) : std::nullopt;
  }
  const auto source = previous.sources.find(path);
  return source == previous.sources.end() ? std::nullopt
                                          : std::optional<std::string_view>(*source->second);
}

std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> DevelopmentFiles::CaptureChanges(
    const CompiledSourceSnapshot& previous,
    std::span<const std::string> paths) {
  const absl::flat_hash_set<std::string> changed(paths.begin(), paths.end());
  auto read = ReadKnownFiles(changed, cache_);
  if (!read) {
    return std::unexpected(std::move(read.error()));
  }
  std::vector<frontend::SourceInput> inputs;
  std::vector<std::string> roots;
  bool has_changes = false;
  for (const auto& path : paths) {
    const auto found = read->find(path);
    const auto text =
        found == read->end() ? std::nullopt : std::optional<std::string_view>(found->second);
    if (text) {
      inputs.push_back({.path = found->first, .text = found->second});
    }
    // 未接纳文本也是已捕获状态；重复通知不能抢在下一次真实编辑前重发旧候选。
    if (text == CapturedText(previous, path)) {
      continue;
    }
    has_changes = true;
    if (text) {
      SetPending(path, std::string(*text));
      roots.push_back(path);
    } else {
      SetPending(path, std::nullopt);
    }
  }
  if (!has_changes) {
    return std::nullopt;
  }
  for (const auto& root : roots) {
    auto loaded = ReadWatchedGraph(root, inputs);
    if (!loaded) {
      return std::unexpected(std::move(loaded.error()));
    }
    auto& graph = *loaded;
    for (const auto& source : graph.sources) {
      SetPending(source.path, std::string(source.text));
    }
    if (!graph.diagnostics.empty()) {
      return std::unexpected(std::move(graph.diagnostics));
    }
  }
  return Pending(previous);
}

std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> DevelopmentFiles::Poll(
    std::chrono::milliseconds timeout) {
  const std::scoped_lock lock(mutex_);
  const auto& previous = *accepted_;
  if (rebase_pending_) {
    rebase_pending_ = false;
    return Pending(previous);
  }
  const auto had_pending = !pending_.empty();
  auto captured = CaptureEvents(previous, watcher_->Drain(timeout));
  if (!captured || !*captured) {
    return captured;
  }
  const auto& update = **captured;
  // efsw 可以重复通知同一次保存；仍需发送把未接纳编辑撤销到基线的空更新。
  if (!had_pending && update.changes.empty() && update.added_roots.empty()) {
    return std::nullopt;
  }
  return captured;
}

std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> DevelopmentFiles::CaptureEvents(
    const CompiledSourceSnapshot& previous,
    const FileChanges& events) {
  if (events.invalidated) {
    const auto rebound = watcher_->Rebind();
    if (!rebound) {
      const auto& error = rebound.error();
      return std::unexpected(FileError(PathText(error.directory), error.message));
    }
    return CaptureAll(previous);
  }
  absl::flat_hash_set<std::string> changed;
  for (const auto& event : events.paths) {
    const auto native = event.path.lexically_normal();
    if (event.kind == FileChangeKind::Identity) {
      const auto refreshed = RefreshDirectory(native, changed);
      if (!refreshed) {
        return std::unexpected(refreshed.error());
      }
    }
    // OS 名字先按 native path 筛选；无关文件不必解码成语言使用的 UTF-8 URI。
    const auto found = native_paths_.find(native);
    if (found == native_paths_.end()) {
      continue;
    }
    const auto& path = found->second;
    changed.insert(path);
    if (!previous.sources.contains(path)) {
      if (const auto roots = group_users_.find(path); roots != group_users_.end()) {
        changed.insert(roots->second.begin(), roots->second.end());
      }
    }
  }
  if (changed.empty()) {
    return std::nullopt;
  }
  if (std::ranges::any_of(changed, [&](const auto& path) { return aliases_.contains(path); })) {
    return CaptureAll(previous);
  }
  return CaptureChanges(previous, std::vector<std::string>(changed.begin(), changed.end()));
}

std::expected<void, frontend::Diagnostics> DevelopmentFiles::RefreshDirectory(
    const std::filesystem::path& native,
    absl::flat_hash_set<std::string>& changed) {
  const auto directory = directory_sources_.find(native);
  if (directory == directory_sources_.end()) {
    return {};
  }
  const auto users = directory->second;
  // Windows 的目录句柄在改名后仍指向旧身份；先退休该订阅，再按当前路径重建。
  watcher_->Unwatch(native);
  for (const auto& path : users) {
    const auto watched = Track(path);
    if (!watched) {
      return watched;
    }
    changed.insert(path);
    if (const auto roots = group_users_.find(path); roots != group_users_.end()) {
      changed.insert(roots->second.begin(), roots->second.end());
    }
  }
  return {};
}

}  // namespace vela::compiler
