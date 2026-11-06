#include "benchmarks/aot_benchmark_kernels.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vela::benchmarks {
namespace {

std::int64_t Step(std::int64_t value) {
  return value + 1;
}

}  // namespace

std::vector<std::int64_t> PrepareNumericValues(NumericInput input) {
  constexpr std::int64_t kMultiplier = 1664525;
  constexpr std::int64_t kIncrement = 1013904223;
  constexpr std::int64_t kStateMask = 0x7fffffff;
  constexpr std::int64_t kValueMask = 1023;
  std::vector<std::int64_t> values;
  values.reserve(static_cast<std::size_t>(input.size));
  auto seed = input.seed;
  for (std::int64_t index = 0; index < input.size; ++index) {
    seed = ((seed * kMultiplier) + kIncrement) & kStateMask;
    values.push_back(seed & kValueMask);
  }
  return values;
}

std::int64_t CppArithmetic(std::span<const std::int64_t> values) {
  std::int64_t sum = 0;
  for (const auto value : values) {
    sum += value * value;
  }
  return sum;
}

std::int64_t CppCalls(std::span<const std::int64_t> values) {
  std::int64_t sum = 0;
  for (const auto value : values) {
    sum += Step(value);
  }
  return sum;
}

[[clang::noinline]] std::int64_t CppScalarEntry(std::int64_t value) {
  return value + 1;
}

}  // namespace vela::benchmarks
