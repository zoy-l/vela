#include "base/directory_watcher.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vela {

std::string internal::DirectoryWatchErrorMessage(std::string_view operation, int error) {
#ifdef _WIN32
  const auto& category = std::system_category();
#else
  const auto& category = std::generic_category();
#endif
  return std::string(operation) + ": " + std::error_code(error, category).message();
}

DirectoryWatcher::DirectoryWatcher(std::function<void()> changed) : changed_(std::move(changed)) {}

DirectoryWatcher::~DirectoryWatcher() {
  CloseNative();
}

bool DirectoryWatcher::IsWatching(const std::filesystem::path& directory,
                                  DirectoryWatchScope scope) const {
  const std::scoped_lock lock(native_mutex_);
  const auto found = subscriptions_.find(directory);
  return found != subscriptions_.end() && found->second.Covers(scope);
}

std::expected<void, std::string> DirectoryWatcher::Watch(const std::filesystem::path& directory,
                                                         DirectoryWatchScope scope) {
  std::unique_lock lock(native_mutex_);
  const auto found = subscriptions_.find(directory);
  if (found != subscriptions_.end()) {
    if (found->second.Covers(scope)) {
      return {};
    }
    RemoveNative(found->second.subscription_id);
    subscriptions_.erase(found);
    // 订阅范围变化期间，完整捕获补齐旧订阅无法观察到的后代变化。
    Invalidate();
  }
#ifndef __APPLE__
  if (failure_) {
    return std::unexpected(*failure_);
  }
#endif
  const auto subscription_id = next_subscription_++;
  auto added = AddNative(directory, scope, subscription_id);
#ifdef __linux__
  if (added && scope == DirectoryWatchScope::Descendants) {
    // 先订阅根以覆盖拓扑变化；遍历文件系统时允许读取线程继续消费其他订阅。
    lock.unlock();
    added = AddNativeDescendants(directory, subscription_id);
    lock.lock();
  }
#endif
  if (!added) {
    RemoveNative(subscription_id);
    return added;
  }
#ifndef __APPLE__
  if (!reader_.joinable()) {
    reader_ = std::thread([this] { ReadEvents(); });
  }
#endif
  subscriptions_.emplace(directory,
                         Subscription{.subscription_id = subscription_id, .scope = scope});
  return {};
}

void DirectoryWatcher::Unwatch(const std::filesystem::path& directory) {
  const std::scoped_lock lock(native_mutex_);
  const auto found = subscriptions_.find(directory);
  if (found != subscriptions_.end()) {
    RemoveNative(found->second.subscription_id);
    subscriptions_.erase(found);
  }
}

std::expected<void, DirectoryWatchError> DirectoryWatcher::Rebind() {
  decltype(subscriptions_) subscriptions;
  {
    const std::scoped_lock lock(native_mutex_);
    subscriptions = std::exchange(subscriptions_, {});
    for (const auto& [directory, subscription] : subscriptions) {
      static_cast<void>(directory);
      RemoveNative(subscription.subscription_id);
    }
  }
  std::optional<DirectoryWatchError> failure;
  for (const auto& [directory, subscription] : subscriptions) {
    std::error_code error;
    const auto exists = std::filesystem::exists(directory, error);
    if (error) {
      if (!failure) {
        failure = DirectoryWatchError{.directory = directory, .message = error.message()};
      }
      continue;
    }
    if (exists) {
      const auto watched = Watch(directory, subscription.scope);
      if (!watched && !failure) {
        failure = DirectoryWatchError{.directory = directory, .message = watched.error()};
      }
    }
  }
  if (failure) {
    return std::unexpected(std::move(*failure));
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

void DirectoryWatcher::Record(std::vector<FileChange>& changes) {
  const std::scoped_lock lock(mutex_);
  if (changes_.invalidated || changes.empty()) {
    return;
  }
  constexpr std::size_t kMaximumPendingEvents = 4096;
  const auto notify = changes_.paths.empty();
  if (changes.size() > kMaximumPendingEvents - changes_.paths.size()) {
    changes_.paths.clear();
    changes_.invalidated = true;
  } else {
    changes_.paths.insert(changes_.paths.end(), std::make_move_iterator(changes.begin()),
                          std::make_move_iterator(changes.end()));
  }
  if (notify) {
    NotifyChanged();
  }
}

void DirectoryWatcher::Invalidate() {
  const std::scoped_lock lock(mutex_);
  const auto notify = !changes_.invalidated && changes_.paths.empty();
  changes_.paths.clear();
  changes_.invalidated = true;
  if (notify) {
    NotifyChanged();
  }
}

void DirectoryWatcher::NotifyChanged() {
  available_.notify_one();
  if (changed_) {
    changed_();
  }
}

}  // namespace vela
