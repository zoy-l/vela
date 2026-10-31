#include <array>
#include <barrier>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "base/file.h"
#include "base/path.h"
#include "base/testing/directory.h"

namespace vela {
namespace {

struct FileVersions final {
  std::vector<std::byte> first;
  std::vector<std::byte> second;
};

std::expected<void, std::string> ReadVersions(const std::filesystem::path& path,
                                              const FileVersions& versions,
                                              int count) {
  for (int index = 0; index < count; ++index) {
    const auto result = ReadBinaryFile(path);
    if (!result) {
      return std::unexpected(result.error());
    }
    if (*result != versions.first && *result != versions.second) {
      return std::unexpected("read a partial publication");
    }
  }
  return {};
}

class FileTest : public test::DirectoryTest {
 protected:
  void ExpectFileCount(std::size_t count) {
    std::error_code error;
    const std::filesystem::directory_iterator entries(directory(), error);
    ASSERT_FALSE(error);
    EXPECT_EQ(
        static_cast<std::size_t>(std::distance(entries, std::filesystem::directory_iterator())),
        count);
  }
};

TEST_F(FileTest, RoundTripsUnicodeEmptyFilesAndReplacesExistingContents) {
  const auto path = directory() / Utf8Path("你好.vbc");
  const auto bytes = std::to_array<std::byte>({std::byte{0}, std::byte{255}});
  ASSERT_TRUE(WriteFileAtomically(path, bytes));
  EXPECT_EQ(ReadBinaryFile(path), std::vector(bytes.begin(), bytes.end()));
  ASSERT_TRUE(WriteFileAtomically(path, {}));
  const auto empty = ReadBinaryFile(path);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->empty());
  EXPECT_EQ(PathText(Utf8Path("你好/世界")), "你好/世界");
}

TEST_F(FileTest, FailingReplacementLeavesNoTemporaryFiles) {
  const auto destination = directory() / "existing";
  std::error_code error;
  ASSERT_TRUE(std::filesystem::create_directory(destination, error));
  ASSERT_FALSE(error);
  EXPECT_FALSE(WriteFileAtomically(destination, {}));
  EXPECT_FALSE(WriteFileAtomically(directory() / "missing" / "output", {}));
  EXPECT_FALSE(ReadBinaryFile(directory() / "absent"));
  const std::filesystem::directory_iterator entries(directory(), error);
  ASSERT_FALSE(error);
  EXPECT_EQ(std::distance(entries, std::filesystem::directory_iterator()), 1);
}

TEST_F(FileTest, PublishesExecutablePermissionsBeforeReplacement) {
  const auto path = directory() / "application";
  ASSERT_TRUE(WriteFileAtomically(path, {}, FileMode::kExecutable));
#ifndef _WIN32
  constexpr auto kExecute = std::filesystem::perms::owner_exec |
                            std::filesystem::perms::group_exec |
                            std::filesystem::perms::others_exec;
  std::error_code error;
  EXPECT_EQ(std::filesystem::status(path, error).permissions() & kExecute, kExecute);
  EXPECT_FALSE(error);
  ASSERT_TRUE(WriteFileAtomically(path, {}));
  EXPECT_EQ(std::filesystem::status(path, error).permissions() & kExecute,
            std::filesystem::perms::none);
  EXPECT_FALSE(error);
#endif
  ExpectFileCount(1);
}

TEST_F(FileTest, ConcurrentReplacementsPublishOnlyWholeFiles) {
  const auto path = directory() / "shared";
  const std::vector<std::byte> first(4096, std::byte{1});
  const std::vector<std::byte> second(8192, std::byte{2});
  ASSERT_TRUE(WriteFileAtomically(path, first));
  std::expected<void, std::string> left_result;
  std::expected<void, std::string> right_result;
  std::jthread left([&]() -> void { left_result = WriteFileAtomically(path, first); });
  std::jthread right([&]() -> void { right_result = WriteFileAtomically(path, second); });
  left.join();
  right.join();
  EXPECT_TRUE(left_result) << left_result.error();
  EXPECT_TRUE(right_result) << right_result.error();
  const auto result = ReadBinaryFile(path);
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result == first || *result == second);
  ExpectFileCount(1);
}

TEST_F(FileTest, ConcurrentReadersSeeOnlyWholePublishedSnapshots) {
  constexpr std::size_t kSnapshotBytes = std::size_t{1024} * 1024;
  constexpr int kPublicationCount = 32;
  const auto path = directory() / "snapshot";
  const FileVersions versions{
      .first = std::vector<std::byte>(kSnapshotBytes, std::byte{1}),
      .second = std::vector<std::byte>(kSnapshotBytes * 2, std::byte{2}),
  };
  ASSERT_TRUE(WriteFileAtomically(path, versions.first));
  std::barrier ready(2);
  std::expected<void, std::string> read;
  std::jthread reader([&]() -> void {
    ready.arrive_and_wait();
    read = ReadVersions(path, versions, kPublicationCount);
  });
  ready.arrive_and_wait();
  for (int index = 0; index < kPublicationCount; ++index) {
    const auto& bytes = index % 2 == 0 ? versions.first : versions.second;
    const auto result = WriteFileAtomically(path, bytes);
    ASSERT_TRUE(result) << result.error();
  }
  reader.join();
  EXPECT_TRUE(read) << read.error();
  ExpectFileCount(1);
}

TEST_F(FileTest, PublishesAllUnicodeNameBufferAlignments) {
  const auto names = std::to_array<std::string_view>({"x", "xx", "xxx", "xxxx", "你好", "你好x"});
  const std::vector<std::byte> bytes(1, std::byte{1});
  for (const auto name : names) {
    const auto path = directory() / Utf8Path(name);
    ASSERT_TRUE(WriteFileAtomically(path, bytes));
    EXPECT_EQ(ReadBinaryFile(path), bytes) << name;
  }
}

}  // namespace
}  // namespace vela
