#pragma once

#include <cstdint>
#include <string_view>

namespace vela::runtime {
struct NativeImage;
}

namespace vela::benchmarks {

enum class AotKernel : std::uint8_t {
  Arithmetic,
  Calls,
  Fields,
  Arrays,
  Host,
  Gc,
  Async,
  SpectralNorm,
};
enum class AotCompletion : std::uint8_t { Immediate, Future };

struct AotBenchmarkCase final {
  std::string_view name;
  AotKernel kernel;
  std::int64_t size;
  std::int64_t expected;
  AotCompletion completion;
  std::uint32_t function;
  std::uint64_t shadow_slots;
  std::uint64_t frame_bytes;
  std::uint64_t root_stores;
};

}  // namespace vela::benchmarks
