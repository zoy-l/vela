#pragma once

#include <functional>
#include <iosfwd>
#include <span>

namespace vela::tooling {

struct FormatStreams final {
  std::reference_wrapper<std::ostream> output;
  std::reference_wrapper<std::ostream> errors;
};

int FormatCommand(std::span<char*> arguments, FormatStreams streams);

}  // namespace vela::tooling
