#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

namespace vela::internal {

struct LinuxDirectoryWatch final {
  std::filesystem::path directory;
  absl::flat_hash_set<std::uint64_t> subscriptions;
  absl::flat_hash_set<std::uint64_t> recursive;
};

struct DirectoryWatchState final {
  int events = -1;
  int wakeup = -1;
  absl::flat_hash_map<int, LinuxDirectoryWatch> watches;
  absl::flat_hash_map<std::filesystem::path, int, std::hash<std::filesystem::path>> paths;
  // 逻辑订阅反向索引，移除只遍历它实际持有的原生 watch。
  absl::flat_hash_map<std::uint64_t, std::vector<int>> registrations;
};

}  // namespace vela::internal
