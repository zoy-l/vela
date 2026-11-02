#pragma once

#include <cstdint>

namespace vela {

enum class TestInspection : std::uint8_t { Kind, Display };

enum class TestControl : std::uint8_t {
  TrackExpectation,
  RegisterCallback,
  EnterCallback,
  ExitCallback,
  CheckCallback,
  CurrentTestScope,
  RegisterFinalizer,
  ReleaseFinalizer,
  TrackCallWait,
};

}  // namespace vela
