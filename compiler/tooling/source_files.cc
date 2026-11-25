#include "compiler/tooling/source_files.h"

#include <algorithm>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace vela::tooling {
namespace {

bool IsExcludedDirectory(const std::filesystem::path& path) {
  const auto name = path.filename();
  return name == ".git" || name == ".cache" || name == ".cipd" || name == "out" ||
         name == "third_party" || name == "node_modules";
}

}  // namespace

std::expected<std::vector<std::filesystem::path>, std::string> DirectorySources(
    const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> sources;
  std::error_code error;
  auto iterator = std::filesystem::recursive_directory_iterator(directory, error);
  const auto end = std::filesystem::recursive_directory_iterator();
  while (!error && iterator != end) {
    const auto& entry = *iterator;
    const auto& path = entry.path();
    const auto status = entry.symlink_status(error);
    if (error) {
      break;
    }
    if (std::filesystem::is_directory(status) && IsExcludedDirectory(path)) {
      iterator.disable_recursion_pending();
    } else if (std::filesystem::is_regular_file(status) && path.extension() == ".vela") {
      sources.push_back(path);
    }
    iterator.increment(error);
  }
  if (error) {
    return std::unexpected(error.message());
  }
  std::ranges::sort(sources);
  return sources;
}

}  // namespace vela::tooling
