#pragma once

#include <memory>

#include <llvm/Support/Error.h>

namespace llvm {
class Triple;
namespace orc {
class LLJIT;
}
}  // namespace llvm

namespace vela::compiler::jit {

// 建立统一的目标、对象链接层、运行时符号和平台展开/调试支持；调用方安装各自的资源计数。
[[nodiscard]] llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>> CreateEngine(
    const llvm::Triple& triple);

}  // namespace vela::compiler::jit
