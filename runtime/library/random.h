#pragma once

#include <array>
#include <cstdint>

namespace vela::runtime {

// 16 字节 isolate-local 状态；xoroshiro128++，SplitMix64 播种，无全局锁。
class Random final {
 public:
  explicit Random(std::uint64_t seed = 0) noexcept;
  void Seed(std::uint64_t seed) noexcept;
  [[nodiscard]] double NextDouble() noexcept;
  // 调用者在用户输入边界验证 bound > 0。
  [[nodiscard]] std::int64_t NextInt(std::int64_t bound) noexcept;

 private:
  [[nodiscard]] std::uint64_t Next() noexcept;
  std::array<std::uint64_t, 2> state_{};
};

}  // namespace vela::runtime
