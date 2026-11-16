#pragma once

#include "runtime/native/native_image.h"

namespace vela::runtime {

[[nodiscard]] NativeHostTarget CurrentNativeHostTarget() noexcept;

}  // namespace vela::runtime
