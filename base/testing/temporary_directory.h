#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace vela::test {

// 只返回本次成功创建的目录；调用方负责在其资源生命周期结束时删除。
[[nodiscard]] std::expected<std::filesystem::path, std::string> CreateTemporaryDirectory();

}  // namespace vela::test
