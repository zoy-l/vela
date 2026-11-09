#include "compiler/testing/discovery.h"

#include <algorithm>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "base/path.h"
#include "base/process_exit.h"
#include "compiler/project/project_config.h"
#include "compiler/testing/pattern.h"
#include "compiler/testing/settings.h"

namespace vela::testing {
namespace {

using Error = compiler::ConfigurationError;

class Discovery final {
 public:
  explicit Discovery(Pattern filename) : filename_(std::move(filename)) {}
  std::expected<void, Error> Add(const std::filesystem::path& path);
  std::vector<std::filesystem::path> Finish(const std::filesystem::path& root);

 private:
  std::expected<void, Error> AddFile(const std::filesystem::path& path);
  Pattern filename_;
  absl::flat_hash_set<std::string> identities_;
  std::vector<std::filesystem::path> paths_;
};

std::expected<void, Error> Discovery::AddFile(const std::filesystem::path& path) {
  std::error_code error;
  auto canonical = std::filesystem::canonical(path, error);
  if (error) {
    return std::unexpected(Error::FileError(path, error.message(), kIoErrorExitCode));
  }
  if (!identities_.insert(PathText(canonical)).second) {
    return {};
  }
  const std::ifstream input(canonical, std::ios::binary);
  if (!input.is_open()) {
    return std::unexpected(Error::FileError(path, "cannot read test source", kIoErrorExitCode));
  }
  paths_.push_back(std::move(canonical));
  return {};
}

std::expected<void, Error> Discovery::Add(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error) {
    return std::unexpected(Error::FileError(path, error.message(), kIoErrorExitCode));
  }
  if (std::filesystem::is_regular_file(status)) {
    if (path.extension() != ".vela") {
      return std::unexpected(
          Error::FileError(path, "test file must have the .vela extension", kUsageExitCode));
    }
    return AddFile(path);
  }
  if (!std::filesystem::is_directory(status)) {
    return std::unexpected(Error::FileError(path, "test entry must be a readable file or directory",
                                            kIoErrorExitCode));
  }
  auto iterator = std::filesystem::recursive_directory_iterator(path, error);
  const auto end = std::filesystem::recursive_directory_iterator();
  while (!error && iterator != end) {
    const auto& entry = *iterator;
    if (entry.is_regular_file(error) && entry.path().extension() == ".vela") {
      const auto matched = filename_.IsMatch(PathText(entry.path().filename()));
      if (!matched) {
        return std::unexpected(Error::FileError(entry.path(), matched.error(), kUsageExitCode));
      }
      if (*matched) {
        const auto added = AddFile(entry.path());
        if (!added) {
          return added;
        }
      }
    }
    if (!error) {
      iterator.increment(error);
    }
  }
  if (error) {
    return std::unexpected(Error::FileError(path, error.message(), kIoErrorExitCode));
  }
  return {};
}

std::vector<std::filesystem::path> Discovery::Finish(const std::filesystem::path& root) {
  std::vector<std::pair<std::string, std::filesystem::path>> ordered;
  ordered.reserve(paths_.size());
  for (auto& path : paths_) {
    auto key = PathText(path.lexically_proximate(root));
    ordered.emplace_back(std::move(key), std::move(path));
  }
  std::ranges::sort(ordered, {}, &decltype(ordered)::value_type::first);
  paths_.clear();
  for (auto& entry : ordered) {
    paths_.push_back(std::move(entry.second));
  }
  return std::move(paths_);
}

}  // namespace

std::expected<std::vector<std::filesystem::path>, compiler::ConfigurationError> DiscoverSuites(
    const compiler::ProjectConfiguration& configuration,
    const Settings& settings,
    const std::filesystem::path& working_directory,
    std::span<const std::string> explicit_paths) {
  auto filename = Pattern::Glob(settings.filename);
  if (!filename) {
    return std::unexpected(Error::FileError(configuration.file, filename.error(), kUsageExitCode));
  }
  Discovery discovery(std::move(*filename));
  const auto paths = explicit_paths.empty() ? std::span(settings.paths) : explicit_paths;
  const auto& base = explicit_paths.empty() ? configuration.root : working_directory;
  if (paths.empty()) {
    return std::unexpected(Error::FileError(
        configuration.file, "test.paths is required when no paths are supplied", kUsageExitCode));
  }
  for (const auto& path : paths) {
    const auto added = discovery.Add(base / Utf8Path(path));
    if (!added) {
      return std::unexpected(added.error());
    }
  }
  return discovery.Finish(configuration.root);
}

}  // namespace vela::testing
