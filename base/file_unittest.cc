#include <barrier>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <array>
#include <string_view>
#endif

#include "gtest/gtest.h"

#include "base/file.h"
#include "base/path.h"
#include "base/testing/directory.h"

namespace vela {
namespace {

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
  const std::vector<std::byte> bytes{std::byte{0}, std::byte{255}};
  ASSERT_TRUE(WriteFileAtomically(path, bytes));
  EXPECT_EQ(ReadBinaryFile(path), bytes);
  ASSERT_TRUE(WriteFileAtomically(path, {}));
  EXPECT_EQ(ReadBinaryFile(path), std::vector<std::byte>{});
  EXPECT_EQ(PathText(Utf8Path("你好/世界")), "你好/世界");
}

TEST_F(FileTest, FailingReplacementLeavesNoTemporaryFiles) {
  const auto& root = directory();
  const auto destination = root / "existing";
  std::error_code error;
  ASSERT_TRUE(std::filesystem::create_directory(destination, error)) << error.message();
  EXPECT_FALSE(WriteFileAtomically(destination, {}));
  EXPECT_FALSE(WriteFileAtomically(root / "missing" / "output", {}));
  EXPECT_FALSE(ReadBinaryFile(root / "absent"));
  ExpectFileCount(1);
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
  std::jthread writer([&] -> void {
    const auto written = WriteFileAtomically(path, first);
    EXPECT_TRUE(written) << written.error();
  });
  const auto written = WriteFileAtomically(path, second);
  EXPECT_TRUE(written) << written.error();
  writer.join();
  const auto result = ReadBinaryFile(path);
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result == first || *result == second);
  ExpectFileCount(1);
}

TEST_F(FileTest, ConcurrentReadersSeeOnlyWholePublishedSnapshots) {
  constexpr std::size_t kSnapshotBytes = std::size_t{1024} * 1024;
  constexpr int kPublicationCount = 32;
  const auto path = directory() / "snapshot";
  const std::vector<std::byte> first(kSnapshotBytes, std::byte{1});
  const std::vector<std::byte> second(kSnapshotBytes * 2, std::byte{2});
  ASSERT_TRUE(WriteFileAtomically(path, first));
  std::barrier ready(2);
  std::jthread reader([&] -> void {
    ready.arrive_and_wait();
    for (int index = 0; index < kPublicationCount; ++index) {
      const auto read = ReadBinaryFile(path);
      ASSERT_TRUE(read) << read.error();
      ASSERT_TRUE(*read == first || *read == second) << "read a partial publication";
    }
  });
  ready.arrive_and_wait();
  for (int index = 0; index < kPublicationCount; ++index) {
    const auto& bytes = index % 2 == 0 ? first : second;
    const auto result = WriteFileAtomically(path, bytes);
    ASSERT_TRUE(result) << result.error();
  }
  reader.join();
  ExpectFileCount(1);
}

#ifdef _WIN32
TEST_F(FileTest, PublishesAllUnicodeNameBufferAlignments) {
  const auto names = std::to_array<std::string_view>({"x", "xx", "xxx", "xxxx", "你好", "你好x"});
  const std::vector<std::byte> bytes(1, std::byte{1});
  for (const auto name : names) {
    const auto path = directory() / Utf8Path(name);
    ASSERT_TRUE(WriteFileAtomically(path, bytes));
    EXPECT_EQ(ReadBinaryFile(path), bytes) << name;
  }
}
#endif

}  // namespace
}  // namespace vela
