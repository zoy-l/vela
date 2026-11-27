#include "base/directory_watcher.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <semaphore>
#include <span>
#include <string>
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
                                             std::span<const std::filesystem::path> paths) {
    absl::flat_hash_set<std::filesystem::path, std::hash<std::filesystem::path>> pending(
        paths.begin(), paths.end());
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

  static testing::AssertionResult AwaitPaths(DirectoryWatcher& watcher,
                                             std::initializer_list<std::filesystem::path> paths) {
    return AwaitPaths(watcher, std::span(paths));
  }

  static testing::AssertionResult CreateDirectory(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::create_directory(path, error)) {
      return testing::AssertionFailure() << path << ": " << error.message();
    }
    return testing::AssertionSuccess();
  }
};

TEST_F(DirectoryWatcherTest, ReportsAtomicPublicationAndRemovalAndStopsIdleReaders) {
  std::counting_semaphore<> changed(0);
  DirectoryWatcher watcher([&changed] { changed.release(); });
  const auto& root = directory();
  ASSERT_TRUE(watcher.Watch(root));
  ASSERT_TRUE(watcher.Watch(root));
  EXPECT_TRUE(watcher.Drain().paths.empty());
  const auto file = root / "source.vela";
  ASSERT_TRUE(WriteFileAtomically(file, {}));
  ASSERT_TRUE(changed.try_acquire_for(std::chrono::seconds(5)));
  ASSERT_TRUE(AwaitPaths(watcher, {file}));
  std::error_code error;
  ASSERT_TRUE(std::filesystem::remove(file, error)) << error.message();
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, RetiredDirectoriesCanBeSubscribedAgain) {
  DirectoryWatcher watcher;
  const auto& root = directory();
  ASSERT_TRUE(watcher.Watch(root));
  watcher.Unwatch(root);
  ASSERT_TRUE(WriteFileAtomically(root / "retired.vela", {}));
  EXPECT_TRUE(watcher.Drain(std::chrono::milliseconds(20)).paths.empty());
  ASSERT_TRUE(watcher.Watch(root));
  const auto file = root / "resubscribed.vela";
  ASSERT_TRUE(WriteFileAtomically(file, {}));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, RemovingOneDirectoryKeepsOtherSubscriptionsActive) {
  const auto& root = directory();
  const auto removed = root / "removed";
  const auto retained = root / "retained";
  std::error_code error;
  ASSERT_TRUE(CreateDirectory(removed));
  ASSERT_TRUE(CreateDirectory(retained));
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(root));
  ASSERT_TRUE(watcher.Watch(removed));
  ASSERT_TRUE(watcher.Watch(retained));
  watcher.Unwatch(removed);
  ASSERT_TRUE(std::filesystem::remove(removed, error)) << error.message();
  ASSERT_TRUE(AwaitPaths(watcher, {removed}));
  const auto file = retained / "source.vela";
  ASSERT_TRUE(WriteFileAtomically(file, {}));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, UnicodeRenamesReportBothSourceAndDestinationPaths) {
  const auto& root = directory();
  const auto original = root / Utf8Path("旧文件.vela");
  const auto renamed = root / Utf8Path("新文件.vela");
  ASSERT_TRUE(WriteFileAtomically(original, {}));
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(root));
  std::error_code error;
  std::filesystem::rename(original, renamed, error);
  ASSERT_FALSE(error) << error.message();
  EXPECT_TRUE(AwaitPaths(watcher, {original, renamed}));
}

TEST_F(DirectoryWatcherTest, UpgradingToRecursiveWatchingInvalidatesTheCapture) {
  const auto& root = directory();
  const auto nested = root / "nested";
  ASSERT_TRUE(CreateDirectory(nested));
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(root));
  ASSERT_TRUE(watcher.Watch(root, DirectoryWatchScope::Descendants));
  EXPECT_TRUE(watcher.Drain().invalidated);
  const auto file = nested / "source.vela";
  ASSERT_TRUE(WriteFileAtomically(file, {}));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, ManyDirectoriesContinueDeliveringEventsAfterRetirement) {
  DirectoryWatcher watcher;
  constexpr std::size_t kDirectories = 128;
  const auto& root = directory();
  std::vector<std::filesystem::path> files;
  for (std::size_t index = 0; index < kDirectories; ++index) {
    const auto path = root / std::to_string(index);
    ASSERT_TRUE(CreateDirectory(path));
    ASSERT_TRUE(watcher.Watch(path));
    files.push_back(path / "source.vela");
  }
  for (const auto& file : files) {
    ASSERT_TRUE(WriteFileAtomically(file, {}));
  }
  ASSERT_TRUE(AwaitPaths(watcher, std::span(files)));
  for (const auto& file : files) {
    const auto path = file.parent_path();
    watcher.Unwatch(path);
    ASSERT_TRUE(watcher.Watch(path));
  }
  const auto file = files.back().parent_path() / "resubscribed.vela";
  ASSERT_TRUE(WriteFileAtomically(file, {}));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, MissingDirectoriesReturnSubscriptionErrors) {
  DirectoryWatcher watcher;
  const auto watched = watcher.Watch(directory() / "missing");
  ASSERT_FALSE(watched);
  EXPECT_FALSE(watched.error().empty());
}

TEST_F(DirectoryWatcherTest, DirectSubscriptionsExcludeNestedFileWrites) {
  const auto& root = directory();
  const auto nested = root / "nested";
  ASSERT_TRUE(CreateDirectory(nested));
  const auto ignored = nested / "ignored.vela";
  const auto observed = root / "observed.vela";
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(root));
  ASSERT_TRUE(WriteFileAtomically(ignored, {}));
  ASSERT_TRUE(WriteFileAtomically(observed, {}));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool received = false;
  while (!received && std::chrono::steady_clock::now() < deadline) {
    const auto events = watcher.Drain(std::chrono::milliseconds(100));
    ASSERT_FALSE(events.invalidated);
    for (const auto& event : events.paths) {
      EXPECT_NE(event.path, ignored);
      received |= event.path == observed;
    }
  }
  EXPECT_TRUE(received);
}

TEST_F(DirectoryWatcherTest, RecursiveSubscriptionsRetainOverlappingDirectWatches) {
  const auto& root = directory();
  const auto nested = root / "nested";
  ASSERT_TRUE(CreateDirectory(nested));
  DirectoryWatcher watcher;
  ASSERT_TRUE(watcher.Watch(root, DirectoryWatchScope::Descendants));
  ASSERT_TRUE(watcher.Watch(nested));
  watcher.Unwatch(root);
  const auto file = nested / "retained.vela";
  ASSERT_TRUE(WriteFileAtomically(file, {}));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

TEST_F(DirectoryWatcherTest, EventOverflowRequestsCompleteCaptureAndRebindRestoresEvents) {
  DirectoryWatcher watcher;
  const auto& root = directory();
  ASSERT_TRUE(watcher.Watch(root));
  // 不消费事件，制造超过队列容量的真实文件创建；同时覆盖原生缓冲区溢出。
  constexpr std::size_t kFileCount = 4200;
  for (std::size_t index = 0; index < kFileCount; ++index) {
    std::ofstream file(root / std::to_string(index));
    ASSERT_TRUE(file);
  }
  constexpr auto kEventWait = std::chrono::milliseconds(100);
  bool invalidated = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!invalidated && std::chrono::steady_clock::now() < deadline) {
    invalidated = watcher.Drain(kEventWait).invalidated;
  }
  ASSERT_TRUE(invalidated);
  ASSERT_TRUE(watcher.Rebind());
  const auto file = root / "after_rebind.vela";
  ASSERT_TRUE(WriteFileAtomically(file, {}));
  EXPECT_TRUE(AwaitPaths(watcher, {file}));
}

}  // namespace
}  // namespace vela
