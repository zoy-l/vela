#pragma once

#include <llvm/Support/Error.h>

namespace llvm::orc {
class LLJIT;
}

namespace vela::compiler::jit {

// 在发布任何代码前安装目标平台的展开和调试注册；注册与 ORC 资源一起退休。
[[nodiscard]] llvm::Error InstallPlatformSupport(llvm::orc::LLJIT& jit);

}  // namespace vela::compiler::jit
