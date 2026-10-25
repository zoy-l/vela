#include "base/file.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include "base/file_publication.h"
#include "base/path.h"

namespace vela {

auto WriteFileAtomically(const std::filesystem::path& path, std::span<const std::byte> bytes)
    -> std::expected<void, std::string> {
  if (std::cmp_greater(bytes.size(), std::numeric_limits<std::streamsize>::max())) {
    return std::unexpected("output exceeds stream size limit");
  }
  static std::atomic<std::uint64_t> sequence = 0;
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  auto temporary = path;
  temporary += ".tmp." + std::to_string(tick) + "." +
               std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
  std::ofstream output(temporary, std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!output) {
    return std::unexpected("cannot create temporary output '" + PathText(temporary) + "'");
  }
  const auto remove_temporary = [&]() -> void {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
  };
  const void* storage = bytes.data();
  if (!bytes.empty()) {
    output.write(static_cast<const char*>(storage), static_cast<std::streamsize>(bytes.size()));
  }
  output.close();
  if (!output) {
    remove_temporary();
    return std::unexpected("cannot write complete output '" + PathText(path) + "'");
  }
  const auto error = internal::PublishFile({.temporary = temporary, .destination = path});
  if (error) {
    remove_temporary();
    return std::unexpected("cannot replace output '" + PathText(path) + "': " + error.message());
  }
  return {};
}

}  // namespace vela
