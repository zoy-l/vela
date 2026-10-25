#include "runtime/random.h"

#include <bit>
#include <cstdint>
#include <expected>

namespace vela::runtime {
namespace {

constexpr std::uint64_t kSeedIncrement = 0x9e3779b97f4a7c15;
constexpr std::uint64_t kSeedMultiplierFirst = 0xbf58476d1ce4e5b9;
constexpr std::uint64_t kSeedMultiplierSecond = 0x94d049bb133111eb;
constexpr int kSeedShiftFirst = 30;
constexpr int kSeedShiftSecond = 27;
constexpr int kSeedShiftFinal = 31;
constexpr int kOutputRotation = 17;
constexpr int kStateRotationFirst = 49;
constexpr int kStateRotationSecond = 28;
constexpr int kStateShift = 21;
constexpr int kMantissaBits = 53;
constexpr int kDiscardedBits = 64 - kMantissaBits;
constexpr double kUnitScale = 0x1.0p-53;

std::uint64_t SplitMix(std::uint64_t& seed) noexcept {
  auto value = (seed += kSeedIncrement);
  value = (value ^ (value >> kSeedShiftFirst)) * kSeedMultiplierFirst;
  value = (value ^ (value >> kSeedShiftSecond)) * kSeedMultiplierSecond;
  return value ^ (value >> kSeedShiftFinal);
}

}  // namespace

Random::Random(std::uint64_t seed) noexcept {
  Seed(seed);
}

void Random::Seed(std::uint64_t seed) noexcept {
  state_.front() = SplitMix(seed);
  state_.back() = SplitMix(seed);
}

std::uint64_t Random::Next() noexcept {
  const auto first = state_.front();
  auto second = state_.back();
  const auto result = std::rotl(first + second, kOutputRotation) + first;
  second ^= first;
  state_.front() = std::rotl(first, kStateRotationFirst) ^ second ^ (second << kStateShift);
  state_.back() = std::rotl(second, kStateRotationSecond);
  return result;
}

double Random::NextDouble() noexcept {
  return static_cast<double>(Next() >> kDiscardedBits) * kUnitScale;
}

std::expected<std::int64_t, RandomError> Random::NextInt(std::int64_t bound) noexcept {
  if (bound <= 0) {
    return std::unexpected(RandomError::NonPositiveBound);
  }
  const auto range = static_cast<std::uint64_t>(bound);
  const auto threshold = (std::uint64_t{0} - range) % range;
  auto value = Next();
  while (value < threshold) {
    value = Next();
  }
  return static_cast<std::int64_t>(value % range);
}

}  // namespace vela::runtime
