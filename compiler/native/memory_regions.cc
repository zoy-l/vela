#include "compiler/native/memory_regions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>

#include "base/declaration.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

llvm::MDNode* NativeMemoryTag(llvm::LLVMContext& context,
                              MemoryRegion region,
                              DeclarationId field) {
  constexpr std::array kNames{
      "heap header",    "class layout",   "field cache",     "instance header",  "field states",
      "managed values", "array elements", "global cache",    "shadow roots",     "GC state",
      "field payloads", "field kinds",    "execution state", "stack root links", "reflection cache",
  };
  static_assert(kNames.size() == static_cast<std::size_t>(MemoryRegion::Count));
  llvm::MDBuilder metadata(context);
  auto* root = metadata.createTBAARoot("Vela native storage");
  auto* type = metadata.createTBAAScalarTypeNode(kNames.at(static_cast<std::size_t>(region)), root);
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
