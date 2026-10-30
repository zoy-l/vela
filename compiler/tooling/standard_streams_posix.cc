#include "compiler/tooling/standard_streams.h"

#include <expected>
#include <string>
#include <unistd.h>

namespace vela::tooling {

bool IsInteractiveStandardOutput() noexcept { return isatty(STDOUT_FILENO) != 0; }

std::expected<void, std::string> SetBinaryStandardStreams() {
  return {};
}

}  // namespace vela::tooling
