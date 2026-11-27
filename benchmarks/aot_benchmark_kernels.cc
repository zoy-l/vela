#include "benchmarks/aot_benchmark_kernels.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

namespace vela::benchmarks {
namespace {

std::int64_t Step(std::int64_t value) {
  return value + 1;
}

}  // namespace

std::vector<std::int64_t> PrepareNumericValues(NumericInput input) {
  constexpr std::uint64_t kMultiplier = 1664525;
  constexpr std::uint64_t kIncrement = 1013904223;
  constexpr std::uint64_t kStateMask = 0x7fffffff;
  constexpr std::uint64_t kValueMask = 1023;
  std::vector<std::int64_t> values;
  values.reserve(static_cast<std::size_t>(input.size));
  auto seed = static_cast<std::uint64_t>(input.seed);
  for (std::int64_t index = 0; index < input.size; ++index) {
    seed = ((seed * kMultiplier) + kIncrement) & kStateMask;
    values.push_back(static_cast<std::int64_t>(seed & kValueMask));
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

std::int64_t CppFlatBodies(std::int64_t size) {
  struct Vec2 final {
    double x;
    double y;
  };
  struct Body final {
    Vec2 position;
    Vec2 velocity;
  };
  std::vector<Body> bodies;
  constexpr double kVerticalSpeed = 2.0;
  bodies.reserve(static_cast<std::size_t>(size));
  for (std::int64_t index = 0; index < size; ++index) {
    bodies.push_back({
        .position = {.x = static_cast<double>(index), .y = static_cast<double>(index * 2)},
        .velocity = {.x = 1.0, .y = kVerticalSpeed},
    });
  }
  benchmark::DoNotOptimize(bodies.data());
  constexpr std::int64_t kFrames = 16;
  constexpr double kDelta = 0.5;
  for (std::int64_t frame = 0; frame < kFrames; ++frame) {
    for (auto& body : bodies) {
      body.position.x += body.velocity.x * kDelta;
      body.position.y += body.velocity.y * kDelta;
    }
  }
  double sum = 0;
  for (const auto& body : bodies) {
    sum += body.position.x + body.position.y;
  }
  return static_cast<std::int64_t>(sum);
}

}  // namespace vela::benchmarks
