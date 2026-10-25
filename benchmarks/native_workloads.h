#pragma once

#include <cstdint>
#include <span>

namespace vela::benchmarks {

using NativeWorkload = std::int64_t (*)(std::span<const std::int64_t>);

std::int64_t Sieve(std::span<const std::int64_t> input);
std::int64_t Mandelbrot(std::span<const std::int64_t> input);
std::int64_t Quicksort(std::span<const std::int64_t> input);
std::int64_t BinaryTrees(std::span<const std::int64_t> input);
std::int64_t Particles(std::span<const std::int64_t> input);
std::int64_t WordCount(std::span<const std::int64_t> input);
std::int64_t ClosurePipeline(std::span<const std::int64_t> input);
std::int64_t ArrayGrowth(std::span<const std::int64_t> input);
std::int64_t ArrayGrowthReserved(std::span<const std::int64_t> input);

}  // namespace vela::benchmarks
