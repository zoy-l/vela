#pragma once

#include <cstdint>
#include <utility>

namespace vela::frontend {

enum class ConditionTruth : std::uint8_t {
  Unknown,
  AlwaysFalse,
  AlwaysTrue,
};

[[nodiscard]] constexpr ConditionTruth NegateConditionTruth(ConditionTruth truth) noexcept {
  switch (truth) {
    case ConditionTruth::Unknown:
      return ConditionTruth::Unknown;
    case ConditionTruth::AlwaysFalse:
      return ConditionTruth::AlwaysTrue;
    case ConditionTruth::AlwaysTrue:
      return ConditionTruth::AlwaysFalse;
  }
  std::unreachable();
}

struct StatementFlow final {
  bool can_fall_through = true;
  bool can_break = false;
  bool can_continue = false;
  ConditionTruth condition = ConditionTruth::Unknown;
};

}  // namespace vela::frontend
