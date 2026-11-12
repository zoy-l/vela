#pragma once

#include <functional>
#include <iosfwd>
#include <span>
#include <string_view>

#include "compiler/testing/settings.h"

namespace vela::runtime {
class NativeInstance;
}

namespace vela::testing {

struct CommandStreams final {
  CommandStreams(std::ostream& standard_output, TerminalMode mode, std::ostream& standard_errors)
      : output(standard_output), errors(standard_errors), terminal(mode) {}

  std::reference_wrapper<std::ostream> output;
  std::reference_wrapper<std::ostream> errors;
  TerminalMode terminal;
};
[[nodiscard]] int TestCommand(std::span<const std::string_view> arguments, CommandStreams streams);
[[nodiscard]] bool HasTestModule(const runtime::NativeInstance& instance);
[[nodiscard]] int RunDirectSuite(runtime::NativeInstance& instance,
                                 std::string_view file,
                                 CommandStreams streams);

}  // namespace vela::testing
