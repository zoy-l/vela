#pragma once

#include <optional>
#include <string_view>

namespace vela::frontend {

struct Program;

inline constexpr std::string_view kSdkSourcePrefix = "sdk:";

// 只有登记的不可变资源可作为 SDK 模块；未知名字不会解析成磁盘路径。
[[nodiscard]] std::optional<std::string_view> SdkModuleSource(std::string_view identity) noexcept;
void BindSdkDeclarations(std::string_view identity, Program& program);

}  // namespace vela::frontend
