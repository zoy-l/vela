#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace vela::benchmarks {

using NativeWorkload = std::int64_t (*)(std::span<const std::int64_t>);

struct WorkloadProblem final {
  std::int64_t size;
  std::int64_t expected;
};

struct Workload final {
  std::string_view name;
  NativeWorkload native;
  std::array<WorkloadProblem, 2> problems;
};

}  // namespace vela::benchmarks
