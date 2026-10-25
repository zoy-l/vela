#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace vela {

struct Fingerprint final {
  std::uint64_t low = 0;
  std::uint64_t high = 0;
  auto operator==(const Fingerprint&) const -> bool = default;
  [[nodiscard]] auto Hex() const -> std::string;
};

// 缓存内容身份使用固定版本的 XXH3-128；与进程随机盐、原生字节序无关。
[[nodiscard]] auto ContentFingerprint(std::span<const std::byte> bytes) noexcept -> Fingerprint;
[[nodiscard]] auto ContentFingerprint(std::string_view text) noexcept -> Fingerprint;

}  // namespace vela
