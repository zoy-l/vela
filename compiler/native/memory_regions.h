#pragma once

#include <cstdint>
#include <string_view>

#include "base/declaration.h"
#include "compiler/ir/ir.h"

namespace llvm {
class LLVMContext;
class MDNode;
}  // namespace llvm

namespace vela::compiler::native {

// 原生存储的互斥区域；拆分字段的载荷与标签使用不同父域。
enum class MemoryRegion : std::uint8_t {
  HeapHeader,
  ClassLayout,
  FieldCache,
  InstanceHeader,
  FieldStates,
  Values,
  Elements,
  GlobalCache,
  Roots,
  GcState,
  FieldPayloads,
  FieldKinds,
  Execution,
  StackRoots,
  ReflectionCache,
  Count,
};

llvm::MDNode* NativeMemoryTag(llvm::LLVMContext& context,
                              MemoryRegion region,
                              DeclarationId field = {});
DeclarationId ClosedFieldDeclaration(const ir::Program& program,
                                     std::string_view type,
                                     ir::FieldId slot);

}  // namespace vela::compiler::native
