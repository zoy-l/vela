#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

namespace llvm {
class Loop;
class Value;
}  // namespace llvm

namespace vela::compiler::native {

// 分块循环自行重建入口及回边，只复用区域复制和出口 PHI 修复。
void CloneLoopBody(llvm::Loop& loop, llvm::ValueToValueMapTy& mapped, llvm::StringRef suffix);

// 复制完整循环区域及出口 PHI，以 condition 分流至副本；调用者使 LoopInfo/支配树失效。
void VersionLoop(llvm::Loop& loop,
                 llvm::Value& condition,
                 llvm::ValueToValueMapTy& mapped,
                 llvm::StringRef suffix);

}  // namespace vela::compiler::native
