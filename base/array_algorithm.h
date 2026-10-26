#pragma once

#include <cstdint>

namespace vela {

enum class ArrayAlgorithm : std::uint8_t {
  Sort,
  ForEach,
  Some,
  Every,
  FindIndex,
  Reduce,
  Map,
};

}  // namespace vela
