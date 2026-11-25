#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <llvm/Support/MemoryBuffer.h>

namespace vela::compiler::jit {

// 把完整输入所有权交给 ORC，payload 可跳过缓存头；目标文件字节无需再复制。
std::unique_ptr<llvm::MemoryBuffer> OwnObjectBytes(std::vector<std::byte> bytes,
                                                   std::size_t offset = 0);

}  // namespace vela::compiler::jit
