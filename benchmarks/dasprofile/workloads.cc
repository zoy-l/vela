#include "benchmarks/dasprofile/workloads.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#pragma clang fp contract(off)

namespace vela::benchmarks::dasprofile {
namespace {

constexpr std::int64_t kDictionaryKeys = 500000;
constexpr std::int64_t kNumbers = 10000;
constexpr std::int64_t kSortElements = 100000;
constexpr std::int64_t kParticles = 50000;
constexpr std::size_t kDimension = 500;
constexpr std::int64_t kQueens = 8;

constexpr auto kShaConstants = std::to_array<std::uint32_t>({
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
});

[[clang::noinline]] void DigestBlock(std::span<const std::uint32_t> data,
                                     std::array<std::uint32_t, 8>& hash) {
  std::array<std::uint32_t, 64> digest{};
  std::ranges::copy(data, digest.begin());
  for (std::size_t i = 16; i < digest.size(); ++i) {
    const auto v0 = digest[i - 15];
    const auto v1 = digest[i - 2];
    const auto s0 = std::rotr(v0, 7) ^ std::rotr(v0, 18) ^ (v0 >> 3);
    const auto s1 = std::rotr(v1, 17) ^ std::rotr(v1, 19) ^ (v1 >> 10);
    digest[i] = digest[i - 16] + s0 + digest[i - 7] + s1;
  }
  auto [a, b, c, d, e, f, g, h] = hash;
  for (std::size_t i = 0; i < digest.size(); ++i) {
    const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    const auto t2 = s0 + majority;
    const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const auto choice = (e & f) ^ ((~e) & g);
    const auto t1 = h + s1 + choice + kShaConstants[i] + digest[i];
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  hash[0] += a;
  hash[1] += b;
  hash[2] += c;
  hash[3] += d;
  hash[4] += e;
  hash[5] += f;
  hash[6] += g;
  hash[7] += h;
}

double Sha256(const std::vector<std::uint32_t>& input) {
  std::array<char, 65> hex{};
  for (std::int64_t round = 0; round < 32; ++round) {
    auto hash = std::to_array<std::uint32_t>({
        0x6a09e667,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19,
    });
    for (std::size_t block = 0; block < input.size() / 16; ++block) {
      DigestBlock(std::span(input).subspan(block * 16, 16), hash);
    }
    constexpr std::string_view kHexDigits = "0123456789abcdef";
    constexpr std::size_t kDigitsPerWord = 8;
    for (std::size_t word = 0; word < hash.size(); ++word) {
      auto value = hash[word];
      for (std::size_t digit = 0; digit < kDigitsPerWord; ++digit) {
        hex[(word * kDigitsPerWord) + kDigitsPerWord - digit - 1] = kHexDigits[value & 0xfU];
        value >>= 4;
      }
    }
    benchmark::DoNotOptimize(hex);
  }
  return std::string_view(hex.data()) ==
                 "8adcaee60bb05a9964a1df12d2f007adcb8f3fa20ff7d1ecfde0a2ac301ff412"
             ? 2048.0
             : -1.0;
}

struct DictionaryHash final {
  std::size_t operator()(std::string_view key) const {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char byte : key) {
      hash = (hash ^ static_cast<unsigned char>(byte)) * 1099511628211ULL;
    }
    return static_cast<std::size_t>(hash);
  }
};

double Dictionary(const std::vector<std::string>& keys) {
  std::unordered_map<std::string_view, std::int64_t, DictionaryHash> counts;
  std::int64_t maximum = 0;
  for (const auto& key : keys) {
    maximum = std::max(maximum, ++counts[key]);
  }
  return static_cast<double>(maximum);
}

struct Body final {
  double x, y, z, padding;
  double vx, vy, vz, mass;
};

double NBodies() {
  constexpr double kSolarMass = 4.0 * std::numbers::pi * std::numbers::pi;
  constexpr double kDays = 365.24;
  auto bodies = std::to_array<Body>({
      {.x = 0, .y = 0, .z = 0, .padding = 0, .vx = 0, .vy = 0, .vz = 0, .mass = kSolarMass},
      {
          .x = 4.84143144246472090,
          .y = -1.16032004402742839,
          .z = -0.103622044471123109,
          .padding = 0,
          .vx = 0.00166007664274403694 * kDays,
          .vy = 0.00769901118419740425 * kDays,
          .vz = -0.0000690460016972063023 * kDays,
          .mass = 0.000954791938424326609 * kSolarMass,
      },
      {
          .x = 8.34336671824457987,
          .y = 4.12479856412430479,
          .z = -0.403523417114321381,
          .padding = 0,
          .vx = -0.00276742510726862411 * kDays,
          .vy = 0.00499852801234917238 * kDays,
          .vz = 0.0000230417297573763929 * kDays,
          .mass = 0.000285885980666130812 * kSolarMass,
      },
      {
          .x = 12.8943695621391310,
          .y = -15.1111514016986312,
          .z = -0.223307578892655734,
          .padding = 0,
          .vx = 0.00296460137564761618 * kDays,
          .vy = 0.00237847173959480950 * kDays,
          .vz = -0.0000296589568540237556 * kDays,
          .mass = 0.0000436624404335156298 * kSolarMass,
      },
      {
          .x = 15.3796971148509165,
          .y = -25.9193146099879641,
          .z = 0.179258772950371181,
          .padding = 0,
          .vx = 0.00268067772490389322 * kDays,
          .vy = 0.00162824170038242295 * kDays,
          .vz = -0.0000951592254519715870 * kDays,
          .mass = 0.0000515138902046611451 * kSolarMass,
      },
  });
  double px = 0;
  double py = 0;
  double pz = 0;
  for (const auto& body : bodies) {
    px += body.vx * body.mass;
    py += body.vy * body.mass;
    pz += body.vz * body.mass;
  }
  bodies[0].vx = -px / kSolarMass;
  bodies[0].vy = -py / kSolarMass;
  bodies[0].vz = -pz / kSolarMass;
  for (std::int64_t step = 0; step < 33000; ++step) {
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      auto& a = bodies[i];
      for (std::size_t j = i + 1; j < bodies.size(); ++j) {
        auto& b = bodies[j];
        const auto dx = a.x - b.x;
        const auto dy = a.y - b.y;
        const auto dz = a.z - b.z;
        const auto squared = (dx * dx) + (dy * dy) + (dz * dz);
        const auto magnitude = 0.01 / (squared * std::sqrt(squared));
        a.vx -= dx * b.mass * magnitude;
        a.vy -= dy * b.mass * magnitude;
        a.vz -= dz * b.mass * magnitude;
        b.vx += dx * a.mass * magnitude;
        b.vy += dy * a.mass * magnitude;
        b.vz += dz * a.mass * magnitude;
      }
    }
    for (auto& body : bodies) {
      body.x += 0.01 * body.vx;
      body.y += 0.01 * body.vy;
      body.z += 0.01 * body.vz;
    }
  }
  double energy = 0;
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    const auto& a = bodies[i];
    energy += 0.5 * a.mass * ((a.vx * a.vx) + (a.vy * a.vy) + (a.vz * a.vz));
    for (std::size_t j = i + 1; j < bodies.size(); ++j) {
      const auto& b = bodies[j];
      const auto dx = a.x - b.x;
      const auto dy = a.y - b.y;
      const auto dz = a.z - b.z;
      energy -= a.mass * b.mass / std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    }
  }
  return energy;
}

enum class Matrix : std::uint8_t { Normal, Transposed };

template <Matrix kMatrix>
void MatrixMultiply(const std::vector<double>& input, std::vector<double>& output) {
  for (std::size_t i = 0; i < kDimension; ++i) {
    double sum = 0;
    for (std::size_t j = 0; j < kDimension; ++j) {
      const auto index = kMatrix == Matrix::Normal ? i : j;
      const auto denominator = ((i + j) * (i + j + 1) / 2) + index + 1;
      sum += input[j] / static_cast<double>(denominator);
    }
    output[i] = sum;
  }
}

double SpectralNorm() {
  std::vector<double> u(kDimension, 1.0);
  std::vector<double> v(kDimension);
  std::vector<double> temporary(kDimension);
  for (std::int64_t round = 0; round < 2; ++round) {
    MatrixMultiply<Matrix::Normal>(u, temporary);
    MatrixMultiply<Matrix::Transposed>(temporary, v);
    MatrixMultiply<Matrix::Normal>(v, temporary);
    MatrixMultiply<Matrix::Transposed>(temporary, u);
  }
  double numerator = 0;
  double denominator = 0;
  for (std::size_t i = 0; i < kDimension; ++i) {
    numerator += u[i] * v[i];
    denominator += v[i] * v[i];
  }
  return std::sqrt(numerator / denominator);
}

std::int64_t MandelbrotLevel(double cx, double cy) {
  std::int64_t count = 0;
  double zx = cx;
  double zy = cy;
  while (std::sqrt((zx * zx) + (zy * zy)) < 2.0 && count < 255) {
    const auto nx = (zx * zx) - (zy * zy);
    const auto ny = (zx * zy) + (zy * zx);
    zx = nx + cx;
    zy = ny + cy;
    ++count;
  }
  return count - 1;
}

double Mandelbrot() {
  std::int64_t sum = 0;
  double x = -2.0;
  for (std::int64_t i = 0; i < 64; ++i) {
    double y = -2.0;
    for (std::int64_t j = 0; j < 64; ++j) {
      sum += MandelbrotLevel(x, y);
      y += 0.0625;
    }
    x += 0.0625;
  }
  return static_cast<double>(sum);
}

double ExpLoop() {
  double sum = 0;
  for (std::int64_t i = 0; i < 1000000; ++i) {
    sum += std::exp(1.0 / (1.0 + static_cast<double>(i)));
  }
  return sum;
}

bool CanPlace(const std::array<std::int64_t, kQueens>& board,
              std::int64_t row,
              std::int64_t column) {
  for (std::int64_t i = 0; i < row; ++i) {
    const auto placed = board[static_cast<std::size_t>(i)];
    if (placed == column || placed - i == column - row || placed + i == column + row) {
      return false;
    }
  }
  return true;
}

std::int64_t AddQueen(std::array<std::int64_t, kQueens>& board, std::int64_t row) {
  if (row == kQueens) {
    return 1;
  }
  std::int64_t solutions = 0;
  for (std::int64_t column = 0; column < kQueens; ++column) {
    if (CanPlace(board, row, column)) {
      board[static_cast<std::size_t>(row)] = column;
      solutions += AddQueen(board, row + 1);
    }
  }
  return solutions;
}

[[clang::noinline]] bool IsPrime(std::int64_t value) {
  for (std::int64_t i = 2; i < value; ++i) {
    if (value % i == 0) {
      return false;
    }
  }
  return true;
}

double Primes() {
  std::int64_t count = 0;
  for (std::int64_t i = 2; i <= 14000; ++i) {
    if (IsPrime(i)) {
      ++count;
    }
  }
  return static_cast<double>(count);
}

class Tree final {
 public:
  bool HasValue(std::int64_t value) {
    NodePtr low;
    NodePtr equal;
    NodePtr high;
    Split(std::move(root_), value, low, equal, high);
    const bool present = equal != nullptr;
    root_ = Merge(Merge(std::move(low), std::move(equal)), std::move(high));
    return present;
  }

  void Insert(std::int64_t value) {
    NodePtr low;
    NodePtr equal;
    NodePtr high;
    Split(std::move(root_), value, low, equal, high);
    if (equal == nullptr) {
      seed_ = ((seed_ * 1103515245) + 12345) & 0x7fffffff;
      equal = std::make_unique<Node>(value, seed_);
    }
    root_ = Merge(Merge(std::move(low), std::move(equal)), std::move(high));
  }

  void Erase(std::int64_t value) {
    NodePtr low;
    NodePtr equal;
    NodePtr high;
    Split(std::move(root_), value, low, equal, high);
    root_ = Merge(std::move(low), std::move(high));
  }

 private:
  struct Node final {
    std::int64_t value;
    std::int64_t priority;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;
  };
  using NodePtr = std::unique_ptr<Node>;

  static NodePtr Merge(NodePtr low, NodePtr high) {
    if (low == nullptr) {
      return high;
    }
    if (high == nullptr) {
      return low;
    }
    if (low->priority < high->priority) {
      low->right = Merge(std::move(low->right), std::move(high));
      return low;
    }
    high->left = Merge(std::move(low), std::move(high->left));
    return high;
  }

  static void Split(NodePtr node, std::int64_t value, NodePtr& low, NodePtr& high) {
    if (node == nullptr) {
      low.reset();
      high.reset();
    } else if (node->value < value) {
      low = std::move(node);
      Split(std::move(low->right), value, low->right, high);
    } else {
      high = std::move(node);
      Split(std::move(high->left), value, low, high->left);
    }
  }

  static void Split(NodePtr node, std::int64_t value, NodePtr& low, NodePtr& equal, NodePtr& high) {
    NodePtr remaining;
    Split(std::move(node), value, low, remaining);
    Split(std::move(remaining), value + 1, equal, high);
  }

  NodePtr root_;
  std::int64_t seed_ = 1;
};

double RunTree() {
  Tree tree;
  std::int64_t current = 5;
  std::int64_t result = 0;
  for (std::int64_t i = 1; i < 20000; ++i) {
    current = ((current * 57) + 43) % 10007;
    switch (i % 3) {
      case 0:
        tree.Insert(current);
        break;
      case 1:
        tree.Erase(current);
        break;
      default:
        result += static_cast<std::int64_t>(tree.HasValue(current));
        break;
    }
  }
  return static_cast<double>(result);
}

std::int64_t Noise(std::int64_t position) {
  auto value = static_cast<std::uint32_t>(position);
  value *= 0x68e31da4U;
  value += 1;
  value ^= value >> 8;
  value += 0xb5297a4dU;
  value ^= value << 8;
  value *= 0x1b56c4e9U;
  value ^= value >> 8;
  return std::bit_cast<std::int32_t>(value);
}

double FibonacciLoop() {
  std::uint32_t last = 1;
  std::uint32_t current = 0;
  for (std::int64_t i = 0; i < 6511134; ++i) {
    const auto temporary = current;
    current += last;
    last = temporary;
  }
  return static_cast<double>(std::bit_cast<std::int32_t>(current));
}

[[clang::noinline]] std::int64_t FibonacciRecursive(std::int64_t value) {
  if (value < 2) {
    return value;
  }
  return FibonacciRecursive(value - 1) + FibonacciRecursive(value - 2);
}

double String2Float(const std::vector<std::string>& texts) {
  double sum = 0;
  for (std::int64_t round = 0; round < 4; ++round) {
    for (const auto& text : texts) {
      const auto* begin = text.data();
      const auto* end = begin + text.size();
      double value = 0;
      const auto result = std::from_chars(begin, end, value);
      if (result.ec != std::errc{} || result.ptr != end) {
        return std::numeric_limits<double>::quiet_NaN();
      }
      sum += value;
    }
  }
  return sum;
}

double Float2String(const std::vector<double>& numbers) {
  std::int64_t sum = 0;
  std::array<char, 128> buffer{};
  auto* begin = buffer.data();
  auto* end = begin + buffer.size();
  for (std::int64_t round = 0; round < 4; ++round) {
    for (const auto value : numbers) {
      const auto result = std::to_chars(begin, end, value);
      if (result.ec != std::errc{}) {
        return std::numeric_limits<double>::quiet_NaN();
      }
      sum += result.ptr - begin;
      benchmark::DoNotOptimize(buffer);
    }
  }
  return static_cast<double>(sum);
}

}  // namespace

CppWorkload::CppWorkload(Workload workload) : workload_(workload) {
  switch (workload) {
    case Workload::Sha256:
      sha_input_.assign(256, 0x2e2e2e2eU);
      break;
    case Workload::Dictionary:
      keys_.reserve(kDictionaryKeys);
      for (std::int64_t i = 0; i < kDictionaryKeys; ++i) {
        keys_.push_back(std::to_string((271828183 ^ (i * 119)) % kDictionaryKeys));
      }
      break;
    case Workload::String2Float:
    case Workload::Float2String:
      numbers_.reserve(kNumbers);
      texts_.reserve(kNumbers);
      for (std::int64_t i = 0; i < kNumbers; ++i) {
        const auto value = static_cast<double>(i) + (static_cast<double>(i) / 10000.0);
        numbers_.push_back(value);
        std::array<char, 128> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        texts_.emplace_back(buffer.data(), result.ptr);
      }
      break;
    case Workload::Particles:
      particles_.reserve(kParticles);
      for (std::int64_t i = 0; i < kParticles; ++i) {
        const auto value = static_cast<double>(i);
        particles_.push_back({.x = value,
                              .y = value + 1,
                              .z = value + 2,
                              .position_padding = 0,
                              .vx = 1,
                              .vy = 2,
                              .vz = 3,
                              .velocity_padding = 0,});
      }
      break;
    case Workload::Sort:
      sort_source_.reserve(kSortElements);
      sort_work_.resize(kSortElements);
      for (std::int64_t i = 0; i < kSortElements; ++i) {
        sort_source_.push_back(Noise(i));
      }
      break;
    default:
      break;
  }
}

double CppWorkload::Run() {
  switch (workload_) {
    case Workload::Sha256:
      return Sha256(sha_input_);
    case Workload::Dictionary:
      return Dictionary(keys_);
    case Workload::NBodies:
      return NBodies();
    case Workload::SpectralNorm:
      return SpectralNorm();
    case Workload::Mandelbrot:
      return Mandelbrot();
    case Workload::ExpLoop:
      return ExpLoop();
    case Workload::String2Float:
      return String2Float(texts_);
    case Workload::Particles: {
      for (std::int64_t round = 0; round < 10; ++round) {
        for (auto& particle : particles_) {
          particle.x += particle.vx;
          particle.y += particle.vy;
          particle.z += particle.vz;
        }
      }
      const auto& last = particles_.back();
      return last.x + last.y + last.z;
    }
    case Workload::Queen: {
      std::array<std::int64_t, kQueens> board{};
      return static_cast<double>(AddQueen(board, 0));
    }
    case Workload::Interop: {
      std::int64_t accumulator = 0;
      for (std::int64_t i = 0; i < 1000000; ++i) {
        accumulator = AddOne(accumulator);
      }
      return static_cast<double>(accumulator);
    }
    case Workload::Primes:
      return Primes();
    case Workload::Tree:
      return RunTree();
    case Workload::Sort: {
      std::ranges::copy(sort_source_, sort_work_.begin());
      std::ranges::stable_sort(sort_work_, std::greater<>());
      if (!std::ranges::is_sorted(sort_work_, std::greater<>())) {
        return std::numeric_limits<double>::quiet_NaN();
      }
      return static_cast<double>(
          std::accumulate(sort_work_.begin(), sort_work_.end(), std::int64_t{0}));
    }
    case Workload::FibonacciLoop:
      return FibonacciLoop();
    case Workload::Float2String:
      return Float2String(numbers_);
    case Workload::FibonacciRecursive:
      return static_cast<double>(FibonacciRecursive(31));
  }
  std::unreachable();
}

}  // namespace vela::benchmarks::dasprofile
