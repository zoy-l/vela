#pragma once

#include <filesystem>

#include <gtest/gtest.h>

namespace vela::test {

// 测试只删除自己成功创建的目录；派生夹具在基类初始化后准备各自输入。
class DirectoryTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

  std::filesystem::path directory_;
};

}  // namespace vela::test
