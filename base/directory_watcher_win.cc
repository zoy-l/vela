#include "base/directory_watcher_win.h"

#include <windows.h>  // IWYU pragma: keep

#include "base/directory_watcher.h"

#include <basetsd.h>
#include <minwinbase.h>
#include <minwindef.h>
#include <winbase.h>

#include <errhandlingapi.h>
#include <fileapi.h>
#include <handleapi.h>
#include <ioapiset.h>
#include <winerror.h>
#include <winnt.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vela::internal {

struct WindowsDirectoryWatch final {
  static constexpr std::size_t kNotificationBufferBytes = std::size_t{64} * 1024;
  static constexpr std::size_t kNotificationBufferWords = kNotificationBufferBytes / sizeof(DWORD);
  using Buffer = std::array<DWORD, kNotificationBufferWords>;
  WindowsDirectoryWatch(std::filesystem::path directory_path, DirectoryWatchScope requested_scope)
      : path(std::move(directory_path)), scope(requested_scope) {}
  WindowsDirectoryWatch(const WindowsDirectoryWatch&) = delete;
  WindowsDirectoryWatch& operator=(const WindowsDirectoryWatch&) = delete;
  WindowsDirectoryWatch(WindowsDirectoryWatch&&) = delete;
  WindowsDirectoryWatch& operator=(WindowsDirectoryWatch&&) = delete;
  ~WindowsDirectoryWatch() {
    if (directory != INVALID_HANDLE_VALUE) {
      CloseHandle(directory);
    }
  }

  HANDLE directory = INVALID_HANDLE_VALUE;
  OVERLAPPED operation{};
  // ReadDirectoryChangesW 的网络目录缓冲区不得超过 64 KiB。
  std::unique_ptr<Buffer> buffer = std::make_unique_for_overwrite<Buffer>();
  std::filesystem::path path;
  DirectoryWatchScope scope = DirectoryWatchScope::Direct;
  bool active = true;
};

DirectoryWatchState::DirectoryWatchState() = default;
DirectoryWatchState::~DirectoryWatchState() = default;

}  // namespace vela::internal

namespace vela {
namespace {

std::string WatchError(std::string_view operation, DWORD error) {
  return internal::DirectoryWatchErrorMessage(operation, static_cast<int>(error));
}

std::expected<void, std::string> Arm(internal::WindowsDirectoryWatch& watch) {
  watch.operation = {};
  constexpr DWORD kChanges = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                             FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
  if (ReadDirectoryChangesW(
          watch.directory, watch.buffer->data(),
          static_cast<DWORD>(internal::WindowsDirectoryWatch::kNotificationBufferBytes),
          static_cast<BOOL>(watch.scope == DirectoryWatchScope::Descendants), kChanges, nullptr,
          &watch.operation, nullptr) == 0) {
    return std::unexpected(WatchError("ReadDirectoryChangesW", GetLastError()));
  }
  return {};
}

// OS 缓冲区在下一次 Arm 前独占；把名字解码成自有路径后才重新交给内核。
std::vector<FileChange> DecodeChanges(const internal::WindowsDirectoryWatch& watch, DWORD bytes) {
  std::vector<FileChange> changes;
  const auto buffer = std::as_bytes(std::span(*watch.buffer));
  std::size_t offset = 0;
  while (offset < bytes) {
    const void* record = &buffer[offset];
    const auto* event = static_cast<const FILE_NOTIFY_INFORMATION*>(record);
    const auto name_bytes = event->FileNameLength;
    const std::wstring_view name(&event->FileName[0], name_bytes / sizeof(wchar_t));
    changes.push_back({
        .path = (watch.path / name).lexically_normal(),
        .kind = event->Action == FILE_ACTION_MODIFIED ? FileChangeKind::Content
                                                      : FileChangeKind::Identity,
    });
    const auto next = event->NextEntryOffset;
    if (next == 0) {
      break;
    }
    offset += next;
  }
  return changes;
}

}  // namespace

std::expected<void, std::string> DirectoryWatcher::AddNative(const std::filesystem::path& directory,
                                                             DirectoryWatchScope scope,
                                                             std::uint64_t subscription_id) {
  if (native_.completion_port == nullptr) {
    auto* const port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (port == nullptr) {
      return std::unexpected(WatchError("CreateIoCompletionPort", GetLastError()));
    }
    native_.completion_port = port;
  }
  auto watch = std::make_unique<internal::WindowsDirectoryWatch>(directory, scope);
  watch->directory =
      CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (watch->directory == INVALID_HANDLE_VALUE) {
    return std::unexpected(WatchError("CreateFileW(directory)", GetLastError()));
  }
  auto* const port = native_.completion_port;
  if (CreateIoCompletionPort(watch->directory, port, static_cast<ULONG_PTR>(subscription_id), 0) ==
      nullptr) {
    return std::unexpected(WatchError("CreateIoCompletionPort(directory)", GetLastError()));
  }
  const auto armed = Arm(*watch);
  if (!armed) {
    return armed;
  }
  native_.watches.emplace(subscription_id, std::move(watch));
  return {};
}

void DirectoryWatcher::RemoveNative(std::uint64_t subscription_id) {
  const auto found = native_.watches.find(subscription_id);
  if (found == native_.watches.end()) {
    return;
  }
  auto& watch = *found->second;
  watch.active = false;
  CancelIoEx(watch.directory, &watch.operation);
}

void DirectoryWatcher::ReadEvents() {
  auto* const port = native_.completion_port;
  while (true) {
    DWORD bytes = 0;
    ULONG_PTR subscription_id = 0;
    OVERLAPPED* operation = nullptr;
    const auto completed =
        GetQueuedCompletionStatus(port, &bytes, &subscription_id, &operation, INFINITE);
    const auto error = (completed != 0) ? ERROR_SUCCESS : GetLastError();
    const std::scoped_lock lock(native_mutex_);
    if (operation != nullptr) {
      const auto found = native_.watches.find(static_cast<std::uint64_t>(subscription_id));
      auto& watch = *found->second;
      if (!watch.active) {
        native_.watches.erase(found);
      } else if (completed == 0) {
        // 删除目录等原生失效由消费者重新订阅并捕获；不保留失效的异步操作。
        Invalidate();
        native_.watches.erase(found);
      } else {
        if (bytes == 0) {
          Invalidate();
        } else {
          auto changes = DecodeChanges(watch, bytes);
          Record(changes);
        }
        if (!Arm(watch)) {
          Invalidate();
          native_.watches.erase(found);
        }
      }
    } else if (completed == 0) {
      failure_ = WatchError("GetQueuedCompletionStatus", error);
      Invalidate();
      return;
    }
    if (stopping_ && native_.watches.empty()) {
      return;
    }
  }
}

void DirectoryWatcher::CloseNative() {
  auto* const port = native_.completion_port;
  if (port == nullptr) {
    return;
  }
  {
    const std::scoped_lock lock(native_mutex_);
    stopping_ = true;
    for (auto& [subscription_id, watch] : native_.watches) {
      static_cast<void>(watch);
      RemoveNative(subscription_id);
    }
  }
  PostQueuedCompletionStatus(port, 0, 0, nullptr);
  if (reader_.joinable()) {
    reader_.join();
  }
  // 仅在 IOCP 自身失败时可能剩余取消结果；仍须等内核归还 OVERLAPPED 的存储。
  for (auto& [subscription_id, watch] : native_.watches) {
    static_cast<void>(subscription_id);
    DWORD bytes = 0;
    GetOverlappedResult(watch->directory, &watch->operation, &bytes, TRUE);
  }
  native_.watches.clear();
  CloseHandle(port);
}

}  // namespace vela
