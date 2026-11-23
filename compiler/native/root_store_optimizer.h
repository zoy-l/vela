#pragma once

#include <string_view>

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

// 同步封闭帧的根槽由本帧写入；GC、被调用脚本及宿主只读取这些非移动引用。
inline constexpr std::string_view kNativeReadOnlyRootsAttribute = "vela.roots.readonly";

void AddRootStorePasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
