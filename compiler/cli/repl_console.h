#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>

namespace vela::tooling {

struct ReplStreams final {
  std::reference_wrapper<std::istream> input;
  std::reference_wrapper<std::ostream> output;
  std::reference_wrapper<std::ostream> errors;
};

[[nodiscard]] int RunRepl(const std::filesystem::path& directory, ReplStreams streams);

}  // namespace vela::tooling
