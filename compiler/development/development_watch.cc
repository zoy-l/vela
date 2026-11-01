#include "compiler/development/development_files.h"

#include <expected>
#include <filesystem>
#include <span>
#include <string>

#include "base/directory_watcher.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::compiler {

std::expected<void, frontend::Diagnostics> DevelopmentFiles::WatchDirectories(
    const std::string& path,
    std::span<const std::filesystem::path> directories) {
  for (const auto& directory : directories) {
    const auto watched = WatchDirectory(path, directory, DirectoryWatchScope::Direct);
    if (!watched) {
      return watched;
    }
  }
  return {};
}

}  // namespace vela::compiler
