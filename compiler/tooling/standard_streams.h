#pragma once

#include <expected>
#include <string>

namespace vela::tooling {

// 协议的长度按原始 UTF-8 字节计数，不能经过 Windows CRT 的换行转换。
[[nodiscard]] std::expected<void, std::string> SetBinaryStandardStreams();
[[nodiscard]] bool IsInteractiveStandardOutput() noexcept;

}  // namespace vela::tooling
