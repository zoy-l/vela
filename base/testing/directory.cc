#include "base/testing/directory.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/file.h"
#include "base/path.h"
#include "base/testing/temporary_directory.h"

namespace vela::test {
void DirectoryTest::SetUp() {
  auto created = CreateTemporaryDirectory();
  ASSERT_TRUE(created) << created.error();
  directory_ = std::move(*created);
}

void DirectoryTest::TearDown() {
  if (!directory_.empty()) {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    EXPECT_FALSE(error) << error.message();
  }
}

void DirectoryTest::Write(std::string_view path, std::string_view source) const {
  ASSERT_TRUE(WriteFileAtomically(directory_ / Utf8Path(path), std::as_bytes(std::span(source))));
}

std::string DirectoryTest::Path(std::string_view path) const {
  return PathText(directory_ / Utf8Path(path));
}

}  // namespace vela::test
