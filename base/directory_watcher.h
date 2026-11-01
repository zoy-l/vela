#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <efsw/efsw.hpp>

namespace vela {

enum class FileChangeKind : std::uint8_t { Content, Identity };

struct FileChange final {
  std::filesystem::path path;
  FileChangeKind kind = FileChangeKind::Content;
};

struct FileChanges final {
  std::vector<FileChange> paths;
  // efsw 报告事件丢失或消费队列溢出时，调用者重新订阅并捕获完整输入快照。
  bool invalidated = false;
};

enum class DirectoryWatchScope : std::uint8_t { Direct, Descendants };

struct DirectoryWatchError final {
  std::filesystem::path directory;
  std::string message;
};

// 只适配 efsw 的订阅和回调；平台后端、读取线程和轮询回退均由 efsw 负责。
// 回调只入队，调用者串行执行 Watch、Unwatch 与 Drain。
class DirectoryWatcher final : private efsw::FileWatchListener {
 public:
  DirectoryWatcher();
  ~DirectoryWatcher() override = default;
  DirectoryWatcher(const DirectoryWatcher&) = delete;
  DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;
  DirectoryWatcher(DirectoryWatcher&&) = delete;
  DirectoryWatcher& operator=(DirectoryWatcher&&) = delete;
  [[nodiscard]] std::expected<void, std::string> Watch(
      const std::filesystem::path& directory,
      DirectoryWatchScope scope = DirectoryWatchScope::Direct);
  void Unwatch(const std::filesystem::path& directory);
  [[nodiscard]] std::expected<void, DirectoryWatchError> Rebind();
  [[nodiscard]] FileChanges Drain(std::chrono::milliseconds timeout = {});

 private:
  struct Subscription final {
    efsw::WatchID id;
    DirectoryWatchScope scope;
  };
  void handleFileAction(efsw::WatchID watch_id,
                        const std::string& directory,
                        const std::string& filename,
                        efsw::Action action,
                        const std::string& old_filename) override;
  void handleMissedFileActions(efsw::WatchID watch_id, const std::string& directory) override;
  void Invalidate();
  std::mutex mutex_;
  std::condition_variable available_;
  FileChanges changes_;
  absl::flat_hash_map<std::filesystem::path, Subscription, std::hash<std::filesystem::path>>
      subscriptions_;
  // 最先析构 efsw 并等待回调结束，再释放 listener 使用的队列和同步对象。
  efsw::FileWatcher watcher_;
};

}  // namespace vela
