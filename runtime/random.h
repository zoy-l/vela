#pragma once

#include <array>
#include <cstdint>
#include <expected>

namespace vela::runtime {

enum class RandomError : std::uint8_t { NonPositiveBound };

// 16 字节 isolate-local 状态；xoroshiro128++，SplitMix64 播种，无全局锁。
class Random final {
 public:
  explicit Random(std::uint64_t seed = 0) noexcept;
  void Seed(std::uint64_t seed) noexcept;
  [[nodiscard]] auto Next() noexcept -> std::uint64_t;
  [[nodiscard]] auto NextDouble() noexcept -> double;
  [[nodiscard]] auto NextInt(std::int64_t bound) noexcept
      -> std::expected<std::int64_t, RandomError>;

 private:
  std::array<std::uint64_t, 2> state_{};
};

}  // namespace vela::runtime
