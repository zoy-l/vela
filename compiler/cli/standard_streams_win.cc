#include "compiler/cli/standard_streams.h"

#include <cerrno>
#include <expected>
#include <string>
#include <system_error>

#include <corecrt_io.h>
#include <fcntl.h>

namespace vela::tooling {

bool IsInteractiveStandardOutput() noexcept {
  return _isatty(1) != 0;
}

std::expected<void, std::string> SetBinaryStandardStreams() {
  constexpr int kStandardInputDescriptor = 0;
  constexpr int kStandardOutputDescriptor = 1;
  if (_setmode(kStandardInputDescriptor, _O_BINARY) == -1 ||
      _setmode(kStandardOutputDescriptor, _O_BINARY) == -1) {
    return std::unexpected("cannot configure binary stdio: " +
                           std::generic_category().message(errno));
  }
  return {};
}

}  // namespace vela::tooling
