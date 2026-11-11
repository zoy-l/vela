#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace vela::test {

// 测试只删除自己成功创建的目录；派生夹具在基类初始化后准备各自输入。
class DirectoryTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;
  void Write(std::string_view path, std::string_view source) const;
  [[nodiscard]] std::string Path(std::string_view path) const;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

 private:
  std::filesystem::path directory_;
};

}  // namespace vela::test
