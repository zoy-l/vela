#pragma once

#include <iosfwd>

#include <cstdint>
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"

namespace vela::tooling {

[[nodiscard]] int RunDebugConsole(runtime::NativeInstance& instance,
                                                  std::uint32_t function,
                                                  std::istream& input,
                                                  std::ostream& output);

}  // namespace vela::tooling
