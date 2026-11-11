#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "runtime/native/native_image.h"

namespace vela::benchmarks::reflection_benchmark {

enum class Registration : std::uint8_t { Disabled, Enabled };

struct Source final {
  std::string_view path;
  std::string_view text;
};

struct QuerySize final {
  std::int64_t background;
  std::int64_t matches;
};

struct ScriptRun final {
  std::string_view path;
  std::string_view entry;
  std::int64_t operations;
  std::int64_t expected;
  Registration registration;
  std::optional<QuerySize> query;
};

struct Program final {
  constexpr Program(std::span<const Source> program_sources,
                    std::span<const ScriptRun> program_cases,
                    const runtime::NativeImage* (*image)() noexcept,
                    std::uint64_t bytes)
      : sources(program_sources), cases(program_cases), load_image(image), object_bytes(bytes) {}
  std::span<const Source> sources;
  std::span<const ScriptRun> cases;
  const runtime::NativeImage* (*load_image)() noexcept;
  std::uint64_t object_bytes;
};

}  // namespace vela::benchmarks::reflection_benchmark
