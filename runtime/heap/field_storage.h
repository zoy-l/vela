#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "runtime/value/value.h"

namespace vela::runtime {

// 对象字段采用连续载荷和独立标签区；跨运行时边界时才重建完整 Value。
// 调用者保证字段数、索引和存储范围有效，且借用不跨字段迁移或可能迁移的安全点。
class FieldStorage final {
 public:
  static constexpr std::size_t kPayloadBytes = sizeof(Value::payload_);
  static constexpr std::size_t kKindBytes = sizeof(ValueKind);

  [[nodiscard]] static constexpr std::size_t KindOffset(std::uint32_t count,
                                                        std::uint32_t index) noexcept {
    return (static_cast<std::size_t>(count) * kPayloadBytes) +
           (static_cast<std::size_t>(index) * kKindBytes);
  }
  [[nodiscard]] static constexpr std::size_t Size(std::uint32_t count) noexcept {
    return static_cast<std::size_t>(count) * (kPayloadBytes + kKindBytes);
  }
  [[nodiscard]] static Value Load(std::span<const std::byte> storage,
                                  std::uint32_t count,
                                  std::uint32_t index) noexcept;
  static void Store(std::span<std::byte> storage,
                    std::uint32_t count,
                    std::uint32_t index,
                    Value value) noexcept;
};

}  // namespace vela::runtime
