#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

namespace llvm::orc {
class LLJIT;
}

namespace vela::compiler::jit {

struct CodeResourceCounts final {
  std::size_t batches = 0;
  std::size_t functions = 0;
  // 链接块实际内容，不含分配器页填充和操作系统调试器开销。
  std::size_t executable_bytes = 0;
  std::size_t read_only_bytes = 0;
  std::size_t writable_bytes = 0;
  // 映像中常量和调试记录的逻辑载荷；属于上述块的组成部分，不能重复相加。
  std::size_t constant_payload_bytes = 0;
  std::size_t debug_payload_bytes = 0;
};

struct CodeResourceCounters final {
  std::atomic_size_t batches{0};
  std::atomic_size_t functions{0};
  std::atomic_size_t executable_bytes{0};
  std::atomic_size_t read_only_bytes{0};
  std::atomic_size_t writable_bytes{0};
  std::atomic_size_t constant_payload_bytes{0};
  std::atomic_size_t debug_payload_bytes{0};
};

// 只核算已成功链接的资源（包括缓存命中）；链接失败和退休会撤销登记。
// 相互独立的 JITDylib 可以并发物化。
void InstallResourceAccounting(llvm::orc::LLJIT& jit,
                               std::shared_ptr<CodeResourceCounters> counters);

}  // namespace vela::compiler::jit
