#pragma once

#include "base/streams.h"

namespace vela::tooling {

// stdio 调用方须先启用 binary 模式；所有协议输出只写 output，日志只写 errors。
[[nodiscard]] int RunLanguageServer(Streams streams);

}  // namespace vela::tooling
