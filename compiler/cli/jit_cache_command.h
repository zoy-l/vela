#pragma once

#include <span>
#include "base/streams.h"

namespace vela::tooling {

// 生成可热重载的开发代码缓存，不链接、初始化或执行脚本。
int JitCacheCommand(std::span<char*> arguments, Streams streams);

}  // namespace vela::tooling
