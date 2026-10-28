#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vela::runtime {
struct NativeImage;
}

namespace vela::benchmarks::dasprofile {

// 顺序对应 Vela prepare 的输入编号。
enum class Workload : std::uint8_t {
  Sha256,
  Dictionary,
  NBodies,
  SpectralNorm,
  Mandelbrot,
  ExpLoop,
  String2Float,
  Particles,
  Queen,
  Interop,
  Primes,
  Tree,
  Sort,
  FibonacciLoop,
  Float2String,
  FibonacciRecursive,
};

struct Case final {
  std::string_view name;
  Workload workload;
  std::int64_t size;
  double expected;
  double increment;
  std::uint32_t function;
  std::uint64_t shadow_slots;
  std::uint64_t frame_bytes;
  std::uint64_t root_stores;
};

class CppWorkload final {
 public:
  explicit CppWorkload(Workload workload);
  [[nodiscard]] double Run();

 private:
  struct Particle final {
    double x, y, z, position_padding;
    double vx, vy, vz, velocity_padding;
  };

  Workload workload_;
  std::vector<std::uint32_t> sha_input_;
  std::vector<std::string> keys_;
  std::vector<double> numbers_;
  std::vector<std::string> texts_;
  std::vector<Particle> particles_;
  std::vector<std::int64_t> sort_source_;
  std::vector<std::int64_t> sort_work_;
};

// 独立翻译单元使跨语言和 C++ 路径调用同一个不可内联函数。
[[nodiscard]] std::int64_t AddOne(std::int64_t value) noexcept;

}  // namespace vela::benchmarks::dasprofile
