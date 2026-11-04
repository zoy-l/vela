#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace vela::frontend {

// 生成源码及缓存重载都必须消费最后提交的清单，拒绝中断发布留下的混代产物。
[[nodiscard]] std::expected<void, std::string> VerifyNativeBindingSource(
    const std::filesystem::path& path,
    std::string_view source);

}  // namespace vela::frontend
