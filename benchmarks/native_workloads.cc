#include "benchmarks/native_workloads.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace vela::benchmarks {
namespace {

constexpr std::int64_t kModulus = 65521;
constexpr std::int64_t kInputMultiplier = 73;
constexpr std::int64_t kInputOffset = 19;
constexpr std::int64_t kMaximumIterations = 50;
constexpr std::int64_t kTreeRounds = 16;
constexpr std::int64_t kFrames = 64;
constexpr std::int64_t kVocabulary = 64;
constexpr std::int64_t kInitialAccumulator = 7;
constexpr std::size_t kDecimalBufferSize = 20;
constexpr double kChecksumScale = 1000.0;
constexpr double kBoxSize = 100.0;
constexpr double kEscapeRadiusSquared = 4.0;
constexpr double kComplexProductScale = 2.0;
constexpr std::int64_t kInitialWidth = 97;
constexpr std::int64_t kInitialHeight = 37;
constexpr double kVelocityX = 0.25;
constexpr double kVelocityY = -0.125;
constexpr double kReflectionExtent = kBoxSize + kBoxSize;

void Sort(std::span<std::int64_t> values, std::int64_t first, std::int64_t last) {
  while (first < last) {
    auto left = first;
    auto right = last;
    const auto pivot = values.subspan(static_cast<std::size_t>((first + last) / 2)).front();
    while (left <= right) {
      while (values.subspan(static_cast<std::size_t>(left)).front() < pivot) {
        ++left;
      }
      while (values.subspan(static_cast<std::size_t>(right)).front() > pivot) {
        --right;
      }
      if (left <= right) {
        std::swap(values.subspan(static_cast<std::size_t>(left)).front(),
                  values.subspan(static_cast<std::size_t>(right)).front());
        ++left;
        --right;
      }
    }
    // 与脚本端保持相同的分区顺序和 O(log n) 递归深度。
    if (right - first < last - left) {
      if (first < right) {
        Sort(values, first, right);
      }
      first = left;
    } else {
      if (left < last) {
        Sort(values, left, last);
      }
      last = right;
    }
  }
}

struct Node final {
  std::unique_ptr<Node> left;
  std::unique_ptr<Node> right;
};

std::unique_ptr<Node> BuildTree(std::size_t depth) {
  if (depth == 0) {
    return std::make_unique<Node>();
  }
  auto left = BuildTree(depth - 1);
  auto right = BuildTree(depth - 1);
  return std::make_unique<Node>(std::move(left), std::move(right));
}

std::int64_t Count(const Node& node) {
  const auto& left = node.left;
  const auto& right = node.right;
  if (!left || !right) {
    return 1;
  }
  return 1 + Count(*left) + Count(*right);
}

struct Particle final {
  double x;
  double y;
  double vx;
  double vy;

  void Update() {
    x += vx;
    y += vy;
    if (x > kBoxSize) {
      x = kReflectionExtent - x;
      vx = -vx;
    }
    if (x < 0.0) {
      x = -x;
      vx = -vx;
    }
    if (y > kBoxSize) {
      y = kReflectionExtent - y;
      vy = -vy;
    }
    if (y < 0.0) {
      y = -y;
      vy = -vy;
    }
  }
};

auto Accumulator(std::int64_t current) {
  return [current](std::int64_t value) mutable {
    const auto sum = current + value;
    current = sum - ((sum / kModulus) * kModulus);
    return current;
  };
}

std::int64_t GrowArray(std::span<const std::int64_t> input, std::size_t capacity) {
  std::vector<std::int64_t> values;
  values.reserve(capacity);
  for (const auto value : input) {
    values.push_back(value);
  }
  std::int64_t checksum = 0;
  for (const auto value : values) {
    checksum += value;
  }
  for (auto index = input.size(); index > 0; --index) {
    const auto value = values.back();
    values.pop_back();
    if (std::cmp_not_equal(value, index - 1)) {
      return -1;
    }
    checksum += value;
  }
  return checksum;
}

}  // namespace

std::int64_t Sieve(std::span<const std::int64_t> input) {
  const auto size = input.size();
  std::vector<std::int64_t> storage(size, 1);
  const std::span flags(storage);
  for (std::size_t prime = 2; prime * prime < size; ++prime) {
    if (flags.subspan(prime).front() == 1) {
      for (auto multiple = prime * prime; multiple < size; multiple += prime) {
        flags.subspan(multiple).front() = 0;
      }
    }
  }
  std::int64_t count = 0;
  for (std::size_t value = 2; value < size; ++value) {
    count += flags.subspan(value).front();
  }
  return count;
}

std::int64_t Mandelbrot(std::span<const std::int64_t> input) {
  const auto size = input.size();
  const double scale = 2.0 / static_cast<double>(size);
  std::int64_t checksum = 0;
  for (std::size_t row = 0; row < size; ++row) {
    const double imaginary = (static_cast<double>(row) * scale) - 1.0;
    for (std::size_t column = 0; column < size; ++column) {
      const double real = (static_cast<double>(column) * scale) - 1.5;
      double z_real = 0.0;
      double z_imaginary = 0.0;
      std::int64_t iterations = 0;
      while (iterations < kMaximumIterations &&
             (z_real * z_real) + (z_imaginary * z_imaginary) <= kEscapeRadiusSquared) {
        const double next = (z_real * z_real) - (z_imaginary * z_imaginary) + real;
        z_imaginary = (kComplexProductScale * z_real * z_imaginary) + imaginary;
        z_real = next;
        ++iterations;
      }
      checksum += iterations;
    }
  }
  return checksum;
}

std::int64_t Quicksort(std::span<const std::int64_t> input) {
  std::vector<std::int64_t> values;
  values.reserve(input.size());
  for (const auto value : input) {
    const auto scaled = (value * kInputMultiplier) + kInputOffset;
    values.push_back(scaled - ((scaled / kModulus) * kModulus));
  }
  const auto size = values.size();
  const std::span sorted(values);
  Sort(sorted, 0, static_cast<std::int64_t>(size) - 1);
  std::int64_t checksum = 0;
  for (std::size_t index = 0; index < size; ++index) {
    const auto value = sorted.subspan(index).front();
    if (index > 0 && sorted.subspan(index - 1).front() > value) {
      return -1;
    }
    checksum += value * static_cast<std::int64_t>(index + 1);
  }
  return checksum;
}

std::int64_t BinaryTrees(std::span<const std::int64_t> input) {
  const auto depth = input.size();
  const auto retained = BuildTree(depth);
  std::int64_t checksum = 0;
  for (std::int64_t iteration = 0; iteration < kTreeRounds; ++iteration) {
    checksum += Count(*BuildTree(depth));
  }
  return checksum + Count(*retained);
}

std::int64_t Particles(std::span<const std::int64_t> input) {
  std::vector<std::unique_ptr<Particle>> particles;
  particles.reserve(input.size());
  for (const auto index : input) {
    const auto initial_x = index - ((index / kInitialWidth) * kInitialWidth);
    const auto initial_y = index - ((index / kInitialHeight) * kInitialHeight);
    particles.push_back(std::make_unique<Particle>(
        static_cast<double>(initial_x), static_cast<double>(initial_y), kVelocityX, kVelocityY));
  }
  for (std::int64_t frame = 0; frame < kFrames; ++frame) {
    for (const auto& particle : particles) {
      particle->Update();
    }
  }
  std::int64_t checksum = 0;
  for (const auto& particle : particles) {
    checksum += static_cast<std::int64_t>(particle->x * kChecksumScale) +
                static_cast<std::int64_t>(particle->y * kChecksumScale);
  }
  return checksum;
}

std::int64_t WordCount(std::span<const std::int64_t> input) {
  // 使用单独的频次向量保留 Map 的首次插入顺序；两端都在计时内构造字符串和结果数组。
  absl::flat_hash_map<std::string, std::size_t> indices;
  std::vector<std::int64_t> counts;
  for (std::size_t index = 0; index < input.size(); ++index) {
    std::array<char, kDecimalBufferSize> buffer{};
    const auto vocabulary = static_cast<std::size_t>(kVocabulary);
    const auto word_index = index - ((index / vocabulary) * vocabulary);
    const auto converted =
        std::to_chars(buffer.data(), std::span(buffer).last(0).data(), word_index);
    std::string line = "event_";
    line.append(buffer.data(), converted.ptr);
    line += ":ok";
    const auto word = line.substr(0, line.find(':'));
    const auto found = indices.find(word);
    if (found == indices.end()) {
      indices.emplace(word, counts.size());
      counts.push_back(1);
    } else {
      ++std::span(counts).subspan(found->second).front();
    }
  }
  const auto frequencies = counts;
  auto checksum = static_cast<std::int64_t>(indices.size());
  for (const auto frequency : frequencies) {
    checksum += frequency * frequency;
  }
  return checksum;
}

std::int64_t ClosurePipeline(std::span<const std::int64_t> input) {
  auto next = Accumulator(kInitialAccumulator);
  std::vector<std::int64_t> mapped;
  mapped.reserve(input.size());
  for (const auto value : input) {
    mapped.push_back(next(value));
  }
  std::int64_t total = 0;
  for (const auto value : mapped) {
    total += value;
  }
  return total;
}

std::int64_t ArrayGrowth(std::span<const std::int64_t> input) {
  return GrowArray(input, 0);
}

std::int64_t ArrayGrowthReserved(std::span<const std::int64_t> input) {
  return GrowArray(input, input.size());
}

}  // namespace vela::benchmarks
