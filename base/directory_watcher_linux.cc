#include "base/directory_watcher.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vela {
namespace {

// 批量读取内核队列，避免为每个文件事件进行单独的系统调用。
constexpr std::size_t kNotificationBufferBytes = 64 * 1024;

std::string WatchError(std::string_view operation, int error) {
  return std::string(operation) + ": " + std::error_code(error, std::generic_category()).message();
}

std::expected<void, std::error_code> Subscribe(internal::DirectoryWatchState& state,
                                               const std::filesystem::path& directory,
                                               DirectoryWatchScope scope,
                                               std::uint64_t subscription_id) {
  int descriptor;
  internal::LinuxDirectoryWatch* subscribed = nullptr;
  const auto existing = state.paths.find(directory);
  if (existing != state.paths.end()) {
    descriptor = existing->second;
    subscribed = &state.watches.at(descriptor);
  } else {
    constexpr std::uint32_t kChanges = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
                                       IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF |
                                       IN_ONLYDIR | IN_DONT_FOLLOW;
    descriptor = inotify_add_watch(state.events, directory.c_str(), kChanges);
    if (descriptor < 0) {
      return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    const auto [watch, inserted] =
        state.watches.try_emplace(descriptor, internal::LinuxDirectoryWatch{
                                                  .directory = directory,
                                                  .subscriptions = {},
                                                  .recursive = {},
                                              });
    if (!inserted && watch->second.directory != directory) {
      return std::unexpected(std::make_error_code(std::errc::file_exists));
    }
    state.paths.emplace(directory, descriptor);
    subscribed = &watch->second;
  }
  auto& watch = *subscribed;
  if (watch.subscriptions.insert(subscription_id).second) {
    state.registrations[subscription_id].push_back(descriptor);
  }
  if (scope == DirectoryWatchScope::Descendants) {
    watch.recursive.insert(subscription_id);
  }
  return {};
}

}  // namespace

std::expected<void, std::string> DirectoryWatcher::AddNative(const std::filesystem::path& directory,
                                                             DirectoryWatchScope scope,
                                                             std::uint64_t subscription_id) {
  if (native_.events < 0) {
    const auto events = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (events < 0) {
      return std::unexpected(WatchError("inotify_init1", errno));
    }
    const auto wakeup = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeup < 0) {
      const auto error = errno;
      close(events);
      return std::unexpected(WatchError("eventfd", error));
    }
    native_.events = events;
    native_.wakeup = wakeup;
  }
  const auto subscribed = Subscribe(native_, directory, scope, subscription_id);
  if (!subscribed) {
    return std::unexpected(WatchError("inotify_add_watch", subscribed.error().value()));
  }
  if (scope == DirectoryWatchScope::Descendants) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(directory, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
      const auto& entry = *iterator;
      // 不追踪目录符号链接，避免递归越出订阅树或形成循环。
      const auto status = entry.symlink_status(error);
      if (!error && std::filesystem::is_directory(status)) {
        const auto added = Subscribe(native_, entry.path(), scope, subscription_id);
        if (!added) {
          if (added.error() == std::errc::no_such_file_or_directory) {
            Invalidate();
          } else {
            return std::unexpected(WatchError("inotify_add_watch", added.error().value()));
          }
        }
      }
      if (error == std::errc::no_such_file_or_directory) {
        Invalidate();
        error.clear();
      }
      if (!error) {
        iterator.increment(error);
      }
    }
    if (error == std::errc::no_such_file_or_directory) {
      Invalidate();
    } else if (error) {
      return std::unexpected(error.message());
    }
  }
  return {};
}

void DirectoryWatcher::RemoveNative(std::uint64_t subscription_id) {
  const auto registration = native_.registrations.find(subscription_id);
  if (registration == native_.registrations.end()) {
    return;
  }
  for (const auto descriptor : registration->second) {
    const auto found = native_.watches.find(descriptor);
    if (found == native_.watches.end()) {
      continue;
    }
    auto& watch = found->second;
    if (watch.subscriptions.erase(subscription_id) == 0) {
      continue;
    }
    watch.recursive.erase(subscription_id);
    if (watch.subscriptions.empty()) {
      native_.paths.erase(watch.directory);
      inotify_rm_watch(native_.events, descriptor);
      // IN_IGNORED 到达前保留退休项，不把迟到的原生事件关联到新的逻辑订阅。
    }
  }
  native_.registrations.erase(registration);
}

void DirectoryWatcher::ReadEvents() {
  alignas(inotify_event) std::array<std::byte, kNotificationBufferBytes> buffer;
  std::vector<FileChange> changes;
  std::array descriptors{pollfd{.fd = native_.events, .events = POLLIN, .revents = 0},
                         pollfd{.fd = native_.wakeup, .events = POLLIN, .revents = 0}};
  while (true) {
    const auto ready = poll(descriptors.data(), descriptors.size(), -1);
    const auto error = errno;
    const std::scoped_lock lock(native_mutex_);
    if (stopping_) {
      return;
    }
    if (ready < 0) {
      if (error == EINTR) {
        continue;
      }
      failure_ = WatchError("poll(inotify)", error);
      Invalidate();
      return;
    }
    if (((descriptors[0].revents | descriptors[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      failure_ = "inotify descriptors became unavailable";
      Invalidate();
      return;
    }
    const auto count = read(native_.events, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        continue;
      }
      failure_ = WatchError("read(inotify)", errno);
      Invalidate();
      return;
    }
    changes.clear();
    const auto bytes = static_cast<std::size_t>(count);
    std::size_t offset = 0;
    while (offset < bytes) {
      const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
      const auto event_bytes = sizeof(inotify_event) + event->len;
      offset += event_bytes;
      if ((event->mask & IN_Q_OVERFLOW) != 0) {
        Invalidate();
        continue;
      }
      const auto found = native_.watches.find(event->wd);
      if (found == native_.watches.end()) {
        continue;
      }
      const auto& watch = found->second;
      if ((event->mask & IN_IGNORED) != 0) {
        if (!watch.subscriptions.empty()) {
          native_.paths.erase(watch.directory);
          Invalidate();
        }
        native_.watches.erase(found);
        continue;
      }
      if (watch.subscriptions.empty()) {
        continue;
      }
      constexpr auto kIdentity = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
      if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0 ||
          ((event->mask & IN_ISDIR) != 0 && (event->mask & kIdentity) != 0 &&
           !watch.recursive.empty())) {
        // inotify 的目录身份跟随 inode。拓扑变化后重建订阅并全量捕获，覆盖新子树的注册窗口。
        Invalidate();
        continue;
      }
      const auto name_bytes = strnlen(event->name, event->len);
      changes.push_back({
          .path = (watch.directory / std::string_view(event->name, name_bytes)).lexically_normal(),
          .kind =
              (event->mask & kIdentity) != 0 ? FileChangeKind::Identity : FileChangeKind::Content,
      });
    }
    Record(changes);
  }
}

void DirectoryWatcher::CloseNative() {
  if (native_.events < 0) {
    return;
  }
  {
    const std::scoped_lock lock(native_mutex_);
    stopping_ = true;
  }
  const std::uint64_t signal = 1;
  ssize_t written;
  do {
    written = write(native_.wakeup, &signal, sizeof(signal));
  } while (written < 0 && errno == EINTR);
  if (reader_.joinable()) {
    reader_.join();
  }
  close(native_.events);
  close(native_.wakeup);
}

}  // namespace vela
