#pragma once

#include <cstdint>
#include <string_view>

namespace vela::runtime {
struct NativeImage;
}

namespace vela::benchmarks {

struct MapLiteralCase final {
  std::string_view name;
  std::string_view source;
  std::int64_t expected;
  std::uint32_t function;
  std::uint64_t shadow_slots;
  std::uint64_t frame_bytes;
  std::uint64_t root_stores;
};

}  // namespace vela::benchmarks
