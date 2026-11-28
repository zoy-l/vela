#include "benchmarks/aot_benchmark_kernels.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

namespace vela::benchmarks {
namespace {

std::int64_t Step(std::int64_t value) {
  return value + 1;
}

struct Vec2 final {
  double x;
  double y;
};

constexpr double kVerticalSpeed = 2.0;

class ClassBody final {
 public:
  explicit ClassBody(Vec2 position) : position_(position), velocity_{.x = 1.0, .y = kVerticalSpeed} {}

  void Step(double delta) {
    position_.x += velocity_.x * delta;
    position_.y += velocity_.y * delta;
  }
  [[nodiscard]] Vec2 position() const { return position_; }

 private:
  Vec2 position_;
  Vec2 velocity_;
};

constexpr std::int64_t kFrames = 16;
constexpr double kDelta = 0.5;

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
  struct Body final {
    Vec2 position;
    Vec2 velocity;
  };
  std::vector<Body> bodies;
  bodies.reserve(static_cast<std::size_t>(size));
  for (std::int64_t index = 0; index < size; ++index) {
    bodies.push_back({
        .position = {.x = static_cast<double>(index), .y = static_cast<double>(index * 2)},
        .velocity = {.x = 1.0, .y = kVerticalSpeed},
    });
  }
  benchmark::DoNotOptimize(bodies.data());
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

std::int64_t CppClassBodyMethods(std::int64_t size) {
  std::vector<std::unique_ptr<ClassBody>> bodies;
  bodies.reserve(static_cast<std::size_t>(size));
  for (std::int64_t index = 0; index < size; ++index) {
    const Vec2 position{.x = static_cast<double>(index), .y = static_cast<double>(index * 2)};
    bodies.push_back(std::make_unique<ClassBody>(position));
  }
  benchmark::DoNotOptimize(bodies.data());
  for (std::int64_t frame = 0; frame < kFrames; ++frame) {
    for (const auto& body : bodies) {
      body->Step(kDelta);
    }
  }
  double sum = 0;
  for (const auto& body : bodies) {
    const auto position = body->position();
    sum += position.x + position.y;
  }
  return static_cast<std::int64_t>(sum);
}

}  // namespace vela::benchmarks
