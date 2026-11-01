#include "base/directory_watcher.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <gtest/gtest.h>

#include "base/file.h"
#include "base/path.h"
#include "base/testing/directory.h"

namespace vela {
namespace {

class DirectoryWatcherTest : public test::DirectoryTest {
 protected:
  static testing::AssertionResult AwaitPaths(DirectoryWatcher& watcher,
                                             std::initializer_list<std::filesystem::path> paths) {
    absl::flat_hash_set<std::filesystem::path, std::hash<std::filesystem::path>> pending(paths);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pending.empty() && std::chrono::steady_clock::now() < deadline) {
      const auto changes = watcher.Drain(std::chrono::milliseconds(10));
      if (changes.invalidated) {
        return testing::AssertionFailure() << "unexpected lost file events";
      }
      for (const auto& change : changes.paths) {
        pending.erase(change.path);
      }
    }
    if (!pending.empty()) {
      return testing::AssertionFailure() << "file event did not arrive: " << *pending.begin();
    }
    return testing::AssertionSuccess();
  }
};

TEST_F(DirectoryWatcherTest, ReportsAtomicPublicationAndRemovalAndStopsIdleReaders) {
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(directory()));
  ASSERT_TRUE(watcher.Watch(directory()));
  EXPECT_TRUE(watcher.Drain().paths.empty());
  const auto file = directory() / "source.vela";
  constexpr std::string_view kSource = "export func value(): int { return 42; }";
  ASSERT_TRUE(WriteFileAtomically(file, std::as_bytes(std::span(kSource))));
  ASSERT_TRUE(AwaitPaths(watcher, {file}));
  std::error_code error;
  ASSERT_TRUE(std::filesystem::remove(file, error));
  ASSERT_FALSE(error) << error.message();
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, RetiredDirectoriesCanBeSubscribedAgain) {
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(directory()));
  watcher.Unwatch(directory());
  constexpr std::string_view kSource = "export func value(): int { return 42; }";
  const auto file = directory() / "source.vela";
  ASSERT_TRUE(WriteFileAtomically(file, std::as_bytes(std::span(kSource))));
  EXPECT_TRUE(watcher.Drain(std::chrono::milliseconds(20)).paths.empty());
  ASSERT_TRUE(watcher.Watch(directory()));
  ASSERT_TRUE(WriteFileAtomically(file, std::as_bytes(std::span(kSource))));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, RemovingOneDirectoryKeepsOtherSubscriptionsActive) {
  const auto removed = directory() / "removed";
  const auto retained = directory() / "retained";
  std::error_code error;
  ASSERT_TRUE(std::filesystem::create_directory(removed, error)) << error.message();
  ASSERT_TRUE(std::filesystem::create_directory(retained, error)) << error.message();
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(directory()));
  ASSERT_TRUE(watcher.Watch(removed));
  ASSERT_TRUE(watcher.Watch(retained));
  watcher.Unwatch(removed);
  ASSERT_TRUE(std::filesystem::remove(removed, error)) << error.message();
  ASSERT_TRUE(AwaitPaths(watcher, {removed}));
  const auto file = retained / "source.vela";
  constexpr std::string_view kSource = "export func value(): int { return 42; }";
  ASSERT_TRUE(WriteFileAtomically(file, std::as_bytes(std::span(kSource))));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, UnicodeRenamesReportBothSourceAndDestinationPaths) {
  const auto original = directory() / Utf8Path("旧文件.vela");
  const auto renamed = directory() / Utf8Path("新文件.vela");
  constexpr std::string_view kSource = "export func value(): int { return 42; }";
  ASSERT_TRUE(WriteFileAtomically(original, std::as_bytes(std::span(kSource))));
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(directory()));
  std::error_code error;
  std::filesystem::rename(original, renamed, error);
  ASSERT_FALSE(error) << error.message();
  EXPECT_TRUE(AwaitPaths(watcher, {original, renamed}));
}

TEST_F(DirectoryWatcherTest, UpgradingToRecursiveWatchingInvalidatesTheCapture) {
  const auto nested = directory() / "nested";
  std::error_code error;
  ASSERT_TRUE(std::filesystem::create_directory(nested, error)) << error.message();
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(directory()));
  ASSERT_TRUE(watcher.Watch(directory(), DirectoryWatchScope::Descendants));
  EXPECT_TRUE(watcher.Drain().invalidated);
  const auto file = nested / "source.vela";
  constexpr std::string_view kSource = "export func value(): int { return 42; }";
  ASSERT_TRUE(WriteFileAtomically(file, std::as_bytes(std::span(kSource))));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, ManyDirectoriesContinueDeliveringEventsAfterRetirement) {
  DirectoryWatcher watcher;
  constexpr std::size_t kDirectories = 128;
  std::vector<std::filesystem::path> directories;
  absl::flat_hash_set<std::filesystem::path, std::hash<std::filesystem::path>> pending;
  for (std::size_t index = 0; index < kDirectories; ++index) {
    const auto path = directory() / std::to_string(index);
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(path, error)) << error.message();
    ASSERT_TRUE(watcher.Watch(path));
    directories.push_back(path);
    pending.insert(path / "source.vela");
  }
  constexpr std::string_view kSource = "export func value(): int { return 42; }";
  for (const auto& file : pending) {
    ASSERT_TRUE(WriteFileAtomically(file, std::as_bytes(std::span(kSource))));
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!pending.empty() && std::chrono::steady_clock::now() < deadline) {
    const auto changes = watcher.Drain(std::chrono::milliseconds(10));
    ASSERT_FALSE(changes.invalidated);
    for (const auto& path : changes.paths) {
      pending.erase(path.path);
    }
  }
  ASSERT_TRUE(pending.empty());
  for (const auto& path : directories) {
    watcher.Unwatch(path);
    ASSERT_TRUE(watcher.Watch(path));
  }
  const auto file = directories.back() / "source.vela";
  ASSERT_TRUE(WriteFileAtomically(file, std::as_bytes(std::span(kSource))));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, MissingDirectoriesReturnSubscriptionErrors) {
  DirectoryWatcher watcher;
  const auto watched = watcher.Watch(directory() / "missing");
  ASSERT_FALSE(watched);
  EXPECT_FALSE(watched.error().empty());
}

}  // namespace
}  // namespace vela
