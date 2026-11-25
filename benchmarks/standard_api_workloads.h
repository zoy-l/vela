#pragma once

#include <cstdint>
#include <span>

namespace vela::benchmarks {

std::int64_t MathAtan2(std::span<const std::int64_t> input);
std::int64_t MathCbrt(std::span<const std::int64_t> input);
std::int64_t MathHypot(std::span<const std::int64_t> input);
std::int64_t MathRemainder(std::span<const std::int64_t> input);
std::int64_t StringBoundary(std::span<const std::int64_t> input);
std::int64_t StringSliceBytes(std::span<const std::int64_t> input);
std::int64_t StringReplaceBytes(std::span<const std::int64_t> input);
std::int64_t StringByteOffset(std::span<const std::int64_t> input);
std::int64_t StringTrim(std::span<const std::int64_t> input);

}  // namespace vela::benchmarks
