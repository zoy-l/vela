#include "compiler/exit_code.h"

#include <expected>
#include <string_view>

#include "runtime/value.h"

namespace vela::compiler {

auto ProcessExitCode(runtime::Value value) noexcept -> std::expected<int, std::string_view> {
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

}  // namespace vela::compiler
