#include "compiler/native/memory_regions.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>

#include "base/declaration.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

bool IsFloatingFieldStore(const llvm::Instruction& instruction) {
  const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
  return store != nullptr && store->getValueOperand()->getType()->isFPOrFPVectorTy() &&
         HasStorageTag(*store, MemoryRegion::FieldPayloads);
}

bool HasStorageTag(const llvm::Instruction& instruction, MemoryRegion region) {
  const auto* tag = instruction.getMetadata(llvm::LLVMContext::MD_tbaa);
  if (tag == nullptr) {
    return false;
  }
  const llvm::StringRef name = MemoryRegionName(region);
  const auto* type = llvm::cast<llvm::MDNode>(tag->getOperand(0));
  // 字段声明的类型节点继承 payload 域；消费别名事实时沿类型父链识别所属存储区域。
  while (type != nullptr) {
    const auto* text = llvm::cast<llvm::MDString>(type->getOperand(0));
    if (text->getString() == name) {
      return true;
    }
    type = type->getNumOperands() > 1 ? llvm::cast<llvm::MDNode>(type->getOperand(1)) : nullptr;
  }
  return false;
}

llvm::MDNode* NativeMemoryTag(llvm::LLVMContext& context,
                              MemoryRegion region,
                              DeclarationId field) {
  llvm::MDBuilder metadata(context);
  auto* root = metadata.createTBAARoot("Vela native storage");
  auto* type = metadata.createTBAAScalarTypeNode(MemoryRegionName(region), root);
  if (field != DeclarationId{} &&
      (region == MemoryRegion::FieldPayloads || region == MemoryRegion::FieldKinds)) {
    // 继承字段共享声明身份；不同声明不会覆盖同一物理字段。通用反射访问仍使用父域。
    type = metadata.createTBAAScalarTypeNode("field " + std::to_string(field.value), type);
  }
  return metadata.createTBAAStructTagNode(type, type, 0);
}

DeclarationId ClosedFieldDeclaration(const ir::Program& program,
                                     std::string_view type,
                                     ir::FieldId slot) {
  const auto found = std::ranges::find(program.classes, type, &ir::Class::name);
  return found == program.classes.end() || slot >= found->fields.size()
             ? DeclarationId{}
             : found->fields[slot].declaration;
}

}  // namespace vela::compiler::native
