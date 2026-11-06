#include "benchmarks/algorithms/workloads.h"

#include <cstdint>

namespace vela::benchmarks::algorithms {

[[clang::noinline]] std::int64_t AddOne(std::int64_t value) noexcept {
  return value + 1;
}

}  // namespace vela::benchmarks::algorithms
