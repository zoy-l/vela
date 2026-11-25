#pragma once

#include <cstdint>
#include <memory>

#include <absl/container/flat_hash_map.h>

namespace vela::internal {

// 每个注册对象独占目录句柄、OVERLAPPED 与缓冲区，取消完成后才释放。
struct WindowsDirectoryWatch;

struct DirectoryWatchState final {
  DirectoryWatchState();
  ~DirectoryWatchState();
  DirectoryWatchState(const DirectoryWatchState&) = delete;
  DirectoryWatchState& operator=(const DirectoryWatchState&) = delete;
  DirectoryWatchState(DirectoryWatchState&&) = delete;
  DirectoryWatchState& operator=(DirectoryWatchState&&) = delete;

  void* completion_port = nullptr;
  absl::flat_hash_map<std::uint64_t, std::unique_ptr<WindowsDirectoryWatch>> watches;
};

}  // namespace vela::internal
