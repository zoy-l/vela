#include "base/directory_watcher.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include <efsw/efsw.hpp>

#include "base/path.h"

namespace vela {

DirectoryWatcher::DirectoryWatcher() {
  watcher_.watch();
}

std::expected<void, std::string> DirectoryWatcher::Watch(const std::filesystem::path& directory,
                                                         DirectoryWatchScope scope) {
  const auto found = subscriptions_.find(directory);
  if (found != subscriptions_.end()) {
    if (scope == DirectoryWatchScope::Direct || found->second.scope == scope) {
      return {};
    }
    // efsw 不接受同路径的重复注册；升级期间的事件由下一次完整捕获补齐。
    Unwatch(directory);
    Invalidate();
  }
  const auto watch_id =
      watcher_.addWatch(PathText(directory), this, scope == DirectoryWatchScope::Descendants);
  if (watch_id < 0) {
    return std::unexpected(efsw::Errors::Log::getLastErrorLog());
  }
  subscriptions_.emplace(directory, Subscription{.id = watch_id, .scope = scope});
  return {};
}

void DirectoryWatcher::Unwatch(const std::filesystem::path& directory) {
  const auto found = subscriptions_.find(directory);
  if (found != subscriptions_.end()) {
    watcher_.removeWatch(found->second.id);
    subscriptions_.erase(found);
  }
}

std::expected<void, DirectoryWatchError> DirectoryWatcher::Rebind() {
  const auto subscriptions = std::exchange(subscriptions_, {});
  for (const auto& [directory, subscription] : subscriptions) {
    static_cast<void>(directory);
    watcher_.removeWatch(subscription.id);
  }
  for (const auto& [directory, subscription] : subscriptions) {
    std::error_code error;
    const auto exists = std::filesystem::exists(directory, error);
    if (error) {
      return std::unexpected(
          DirectoryWatchError{.directory = directory, .message = error.message()});
    }
    if (exists) {
      const auto watched = Watch(directory, subscription.scope);
      if (!watched) {
        return std::unexpected(
            DirectoryWatchError{.directory = directory, .message = watched.error()});
      }
    }
  }
  return {};
}

FileChanges DirectoryWatcher::Drain(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  if (timeout > std::chrono::milliseconds::zero()) {
    available_.wait_for(lock, timeout,
                        [this] { return changes_.invalidated || !changes_.paths.empty(); });
  }
  return std::exchange(changes_, {});
}

void DirectoryWatcher::handleFileAction(efsw::WatchID /*watch_id*/,
                                        const std::string& directory,
                                        const std::string& filename,
                                        efsw::Action action,
                                        const std::string& old_filename) {
  const auto parent = Utf8Path(directory);
  const auto path = (parent / Utf8Path(filename)).lexically_normal();
  const auto kind =
      action == efsw::Actions::Modified ? FileChangeKind::Content : FileChangeKind::Identity;
  const std::scoped_lock lock(mutex_);
  constexpr std::size_t kMaximumPendingEvents = 4096;
  if (changes_.invalidated) {
    return;
  }
  const auto moved = action == efsw::Actions::Moved;
  const std::size_t event_count = moved ? 2 : 1;
  if (changes_.paths.size() + event_count > kMaximumPendingEvents) {
    changes_.paths.clear();
    changes_.invalidated = true;
  } else {
    if (moved) {
      changes_.paths.push_back({
          .path = (parent / Utf8Path(old_filename)).lexically_normal(),
          .kind = FileChangeKind::Identity,
      });
    }
    changes_.paths.push_back({.path = path, .kind = kind});
  }
  available_.notify_one();
}

void DirectoryWatcher::handleMissedFileActions(efsw::WatchID /*watch_id*/,
                                               const std::string& /*directory*/) {
  Invalidate();
}

void DirectoryWatcher::Invalidate() {
  const std::scoped_lock lock(mutex_);
  changes_.paths.clear();
  changes_.invalidated = true;
  available_.notify_one();
}

}  // namespace vela
