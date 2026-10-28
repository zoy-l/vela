#pragma once

#include <cstdint>

#include "runtime/native_image.h"

namespace vela::runtime {

// main 编号由离线编译器验证；映像及代码静态链接，覆盖整个进程生命周期。
extern "C" int vela_native_v1_main(const NativeImage* image, std::uint32_t function);

}  // namespace vela::runtime
