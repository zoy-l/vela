#pragma once

#include <memory>
#include <string_view>

#include "runtime/native/native_function.h"

namespace vela::runtime {

[[nodiscard]] std::shared_ptr<const NativeFunction> FindStandardNative(std::string_view name);

}  // namespace vela::runtime
