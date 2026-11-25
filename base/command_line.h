#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace vela {

// 不匹配或缺少值时由命令返回其用法诊断；空值不能隐式选择当前目录。
std::optional<std::filesystem::path> ReadPathOption(std::string_view argument,
                                                    std::string_view prefix);

}  // namespace vela
