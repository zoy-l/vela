#pragma once

#include <cstdint>

namespace vela {

enum class JSONDecodeKind : std::uint8_t {
  Dynamic,
  Bool,
  Int,
  Double,
  String,
  Nullable,
  Array,
  Map,
  Class,
};

}  // namespace vela
