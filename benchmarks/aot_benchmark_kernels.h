#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vela::benchmarks {

inline constexpr std::int64_t kAotWorkloadSize = 1000;
inline constexpr std::int64_t kAotInputSeed = 42;
inline constexpr std::int64_t kEntryInput = 41;
inline constexpr std::int64_t kEntryExpected = 42;

struct NumericInput final {
  std::int64_t size;
  std::int64_t seed;
};

// 输入准备独立于计时；Vela prepareNumeric 使用相同的有界整数序列。
[[nodiscard]] std::vector<std::int64_t> PrepareNumericValues(NumericInput input);
[[nodiscard]] std::int64_t CppArithmetic(std::span<const std::int64_t> values);
[[nodiscard]] std::int64_t CppCalls(std::span<const std::int64_t> values);

// 仅入口延迟对照保留不可内联边界；算法用例允许编译器正常内联。
[[nodiscard]] std::int64_t CppScalarEntry(std::int64_t value);

}  // namespace vela::benchmarks
