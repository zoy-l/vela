#include "base/testing/temporary_directory.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <system_error>

namespace vela::test {

std::expected<std::filesystem::path, std::string> CreateTemporaryDirectory() {
  static std::atomic<std::uint64_t> sequence{0};
  std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  if (error) {
    return std::unexpected(error.message());
  }
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  auto directory = temporary / ("vela-" + std::to_string(suffix) + '-' +
                                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
  if (!std::filesystem::create_directory(directory, error)) {
    return std::unexpected(error ? error.message() : "temporary directory already exists");
  }
  return directory;
}

}  // namespace vela::test
