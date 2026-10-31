#include "compiler/native/function_emitter.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include "compiler/native/escape_analysis.h"
#include "compiler/native/native_values.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::native {

llvm::Constant* FunctionEmitter::VirtualGraph(std::size_t instruction) {
  const auto& checkpoint = escape_.checkpoints.at(instruction);
  auto* pointer = builder_.getPtrTy();
  if (checkpoint.objects.empty()) {
    return llvm::ConstantPointerNull::get(pointer);
  }
  const auto prefix = generated_.subspan(function_index_).front().body->getName().str() +
                      "_virtual_" + std::to_string(instruction);
  if (auto* existing = module_.getNamedGlobal(prefix)) {
    return existing;
  }
  auto& context = module_.getContext();
  auto* word = builder_.getInt32Ty();
  auto* wide = builder_.getInt64Ty();
  auto* table = llvm::StructType::get(context, {pointer, wide});
  auto* field_type = llvm::StructType::get(context, {wide, word, word, builder_.getInt8Ty()});
  auto* object_type = llvm::StructType::get(context, {word, word, word, table});
  auto* alias_type = llvm::StructType::get(context, {word, word});
  std::vector<std::uint32_t> indices(escape_.allocations.size(), runtime::kNoNativeIndex);
  for (std::size_t index = 0; index < checkpoint.objects.size(); ++index) {
    indices.at(checkpoint.objects.at(index).allocation) = static_cast<std::uint32_t>(index);
  }
  std::vector<llvm::Constant*> objects;
  objects.reserve(checkpoint.objects.size());
  for (const auto& object : checkpoint.objects) {
    const auto& allocation = escape_.allocations.at(object.allocation);
    std::vector<llvm::Constant*> fields;
    fields.reserve(object.fields.size());
    fields.reserve(object.fields.size());
    for (const auto& field : object.fields) {
      fields.push_back(llvm::ConstantStruct::get(
          field_type,
          {
              builder_.getInt64(field.declaration.value),
              builder_.getInt32(field.value),
              builder_.getInt32(field.object ? indices.at(*field.object) : runtime::kNoNativeIndex),
              builder_.getInt8(static_cast<std::uint8_t>(field.initialized
                                                             ? runtime::FieldState::Ready
                                                             : runtime::FieldState::Uninitialized)),
          }));
    }
    const auto fields_name = prefix + "_fields_" + std::to_string(object.allocation);
    objects.push_back(llvm::ConstantStruct::get(
        object_type,
        {
            builder_.getInt32(allocation.type),
            builder_.getInt32(static_cast<std::uint32_t>(object.allocation)),
            builder_.getInt32(static_cast<std::uint32_t>(*roots_.slots.at(allocation.reference))),
            EmitConstantTable(module_, table, field_type, fields, fields_name),
        }));
  }
  std::vector<llvm::Constant*> aliases;
  aliases.reserve(checkpoint.aliases.size());
  aliases.reserve(checkpoint.aliases.size());
  for (const auto& alias : checkpoint.aliases) {
    aliases.push_back(llvm::ConstantStruct::get(
        alias_type,
        {builder_.getInt32(alias.reg), builder_.getInt32(indices.at(alias.allocation))}));
  }
  auto* graph_type = llvm::StructType::get(context, {table, table});
  auto* contents = llvm::ConstantStruct::get(
      graph_type, {
                      EmitConstantTable(module_, table, object_type, objects, prefix + "_objects"),
                      EmitConstantTable(module_, table, alias_type, aliases, prefix + "_aliases"),
                  });
  auto* graph = new llvm::GlobalVariable(module_, graph_type, true,
                                         llvm::GlobalValue::PrivateLinkage, contents, prefix);
  return graph;
}

}  // namespace vela::compiler::native
