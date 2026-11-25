#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <absl/container/flat_hash_map.h>

#ifdef _WIN32
#include "base/directory_watcher_win.h"
#elifdef __APPLE__
#include "base/directory_watcher_mac.h"
#else
#include "base/directory_watcher_linux.h"
#endif

namespace vela {

namespace internal {
std::string DirectoryWatchErrorMessage(std::string_view operation, int error);
}

enum class FileChangeKind : std::uint8_t { Content, Identity };

struct FileChange final {
  std::filesystem::path path;
  FileChangeKind kind = FileChangeKind::Content;
};

struct FileChanges final {
  std::vector<FileChange> paths;
  // 原生事件丢失、递归目录身份变化或队列溢出后，调用者重新订阅并捕获完整输入快照。
  bool invalidated = false;
};

enum class DirectoryWatchScope : std::uint8_t { Direct, Descendants };

struct DirectoryWatchError final {
  std::filesystem::path directory;
  std::string message;
};

// 原生事件只入队，调用者串行执行订阅和消费。无订阅时不启动读取线程；没有扫描回退。
class DirectoryWatcher final {
 public:
  // changed 只通知消费者，不得重入 watcher；调用方可在回调中唤醒自己的执行域。
  explicit DirectoryWatcher(std::function<void()> changed = {});
  ~DirectoryWatcher();
  DirectoryWatcher(const DirectoryWatcher&) = delete;
  DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;
  DirectoryWatcher(DirectoryWatcher&&) = delete;
  DirectoryWatcher& operator=(DirectoryWatcher&&) = delete;
  [[nodiscard]] std::expected<void, std::string> Watch(
      const std::filesystem::path& directory,
      DirectoryWatchScope scope = DirectoryWatchScope::Direct);
  [[nodiscard]] bool IsWatching(const std::filesystem::path& directory,
                                DirectoryWatchScope scope = DirectoryWatchScope::Direct) const;
  void Unwatch(const std::filesystem::path& directory);
  [[nodiscard]] std::expected<void, DirectoryWatchError> Rebind();
  [[nodiscard]] FileChanges Drain(std::chrono::milliseconds timeout = {});

 private:
#ifdef __APPLE__
  friend struct internal::MacDirectoryWatch;
#endif
  struct Subscription final {
    std::uint64_t subscription_id;
    DirectoryWatchScope scope;
    [[nodiscard]] bool Covers(DirectoryWatchScope requested) const {
      return requested == DirectoryWatchScope::Direct || scope == requested;
    }
  };
  [[nodiscard]] std::expected<void, std::string> AddNative(const std::filesystem::path& directory,
                                                           DirectoryWatchScope scope,
                                                           std::uint64_t subscription_id);
#ifdef __linux__
  [[nodiscard]] std::expected<void, std::string> AddNativeDescendants(
      const std::filesystem::path& directory,
      std::uint64_t subscription_id);
#endif
  void RemoveNative(std::uint64_t subscription_id);
  void CloseNative();
#ifndef __APPLE__
  void ReadEvents();
#endif
  void Record(std::vector<FileChange>& changes);
  void Invalidate();
  void NotifyChanged();
  std::mutex mutex_;
  std::condition_variable available_;
  FileChanges changes_;
  const std::function<void()> changed_;
  mutable std::mutex native_mutex_;
  absl::flat_hash_map<std::filesystem::path, Subscription, std::hash<std::filesystem::path>>
      subscriptions_;
  std::uint64_t next_subscription_ = 1;
#ifndef __APPLE__
  std::optional<std::string> failure_;
  bool stopping_ = false;
  std::thread reader_;
#endif
  internal::DirectoryWatchState native_;
};

}  // namespace vela
