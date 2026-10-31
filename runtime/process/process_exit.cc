#include "runtime/process/process_exit.h"

#include <expected>
#include <string_view>

#include "base/process_exit.h"
#include "runtime/value/value.h"

namespace vela::runtime {

std::expected<int, std::string_view> ProcessExitCode(Value value) noexcept {
  if (value.IsNull()) {
    return 0;
  }
  if (!value.IsInteger()) {
    return std::unexpected("main must return an int exit code or void");
  }
  const auto code = value.AsInteger();
  if (code < 0 || code > kMaximumExitCode) {
    return std::unexpected("main exit code must be in the range 0..255");
  }
  return static_cast<int>(code);
}

}  // namespace vela::runtime
