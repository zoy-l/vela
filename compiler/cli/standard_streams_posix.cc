#include "compiler/cli/standard_streams.h"

#include <unistd.h>
#include <expected>
#include <string>

namespace vela::tooling {

bool IsInteractiveStandardOutput() noexcept {
  return isatty(STDOUT_FILENO) != 0;
}

std::expected<void, std::string> SetBinaryStandardStreams() {
  return {};
}

}  // namespace vela::tooling
