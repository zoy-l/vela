#pragma once

#include <cstdint>
#include <memory>

#include <absl/container/flat_hash_map.h>

namespace vela::internal {

// stream 回调借用注册对象；Stop/Invalidate 和队列屏障结束后才能释放。
struct MacDirectoryWatch;

struct DirectoryWatchState final {
  DirectoryWatchState();
  ~DirectoryWatchState();
  DirectoryWatchState(const DirectoryWatchState&) = delete;
  DirectoryWatchState& operator=(const DirectoryWatchState&) = delete;
  DirectoryWatchState(DirectoryWatchState&&) = delete;
  DirectoryWatchState& operator=(DirectoryWatchState&&) = delete;

  std::uintptr_t queue = 0;
  absl::flat_hash_map<std::uint64_t, std::unique_ptr<MacDirectoryWatch>> watches;
};

}  // namespace vela::internal
