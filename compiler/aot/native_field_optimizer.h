#pragma once

namespace llvm {
class Module;
class TargetMachine;
}

namespace vela::compiler::aot {

// 将可交换的整数组重复更新合并为每个对象上的顺序更新；不重排同一字段的浮点加法。
void FoldRepeatedFieldUpdates(llvm::Module& module, llvm::TargetMachine& machine);

}  // namespace vela::compiler::aot
