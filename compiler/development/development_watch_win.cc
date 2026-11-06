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
  if (directories.size() > 1) {
    const auto parent = WatchDirectory(path, directories.front(), DirectoryWatchScope::Direct);
    if (!parent) {
      return parent;
    }
  }
  return WatchDirectory(path, directories[directories.size() > 1 ? 1 : 0],
                        DirectoryWatchScope::Descendants);
}

}  // namespace vela::compiler
