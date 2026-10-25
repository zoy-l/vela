#pragma once

#include <memory>
#include <string_view>

#include "runtime/native_function.h"

namespace vela::runtime {

[[nodiscard]] auto FindStandardNative(std::string_view name)
    -> std::shared_ptr<const NativeFunction>;

}  // namespace vela::runtime
