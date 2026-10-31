#pragma once

#include <cstdint>

namespace vela::frontend {

enum class ConditionTruth : std::uint8_t {
  Unknown,
  AlwaysFalse,
  AlwaysTrue,
};

struct StatementFlow final {
  bool can_fall_through = true;
  bool can_break = false;
  bool can_continue = false;
  ConditionTruth condition = ConditionTruth::Unknown;
};

}  // namespace vela::frontend
