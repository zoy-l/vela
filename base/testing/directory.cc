#include "base/testing/directory.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>
#include <utility>

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

}  // namespace vela::test
