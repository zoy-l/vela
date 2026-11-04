#include "compiler/native/native_image_emitter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include "base/declaration.h"
#include "base/source_location.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::native {
namespace {

using runtime::NativeArrayDescription;
using runtime::NativeBindingDependency;
using runtime::NativeCallParameter;
using runtime::NativeClassDescription;
using runtime::NativeCollectionDescription;
using runtime::NativeConstructionDescription;
using runtime::NativeDeclarationDescription;
using runtime::NativeDependencyBlock;
using runtime::NativeFieldDescription;
using runtime::NativeFunctionDependencies;
using runtime::NativeFunctionDescription;
using runtime::NativeImage;
using runtime::NativeJSONDecodeParameter;
using runtime::NativeJSONDecodePlan;
using runtime::NativeMemberDescription;
using runtime::NativeSignatureDescription;
using runtime::NativeSignatureParameter;
using runtime::NativeTable;
using runtime::NativeText;
using runtime::NativeTypeDescription;

class ImageEmitter final {
 public:
  explicit ImageEmitter(llvm::Module& module);
  ~ImageEmitter() = default;
  ImageEmitter(const ImageEmitter&) = delete;
  ImageEmitter& operator=(const ImageEmitter&) = delete;
  ImageEmitter(ImageEmitter&&) = delete;
  ImageEmitter& operator=(ImageEmitter&&) = delete;
  std::expected<llvm::Constant*, std::string> Emit(const NativeImage& image,
                                                   std::span<llvm::Function* const> entries);

 private:
  template <class Record, std::size_t Count>
  llvm::StructType* RecordType(std::string_view name,
                               const std::array<llvm::Type*, Count>& fields,
                               const std::array<std::size_t, Count>& offsets) {
    static_assert(std::is_standard_layout_v<Record>);
    auto* type = llvm::StructType::create(context_, fields, name);
    const auto* layout = module_.getDataLayout().getStructLayout(type);
    const auto indices = std::views::iota(std::size_t{0}, Count);
    const bool matches =
        module_.getDataLayout().getTypeAllocSize(type) == sizeof(Record) &&
        std::ranges::all_of(indices, [&](std::size_t index) {
          return layout->getElementOffset(static_cast<unsigned>(index)) == offsets.at(index);
        });
    has_compatible_layout_ = has_compatible_layout_ && matches;
    return type;
  }
  static llvm::Constant* Record(llvm::StructType* type,
                                std::initializer_list<llvm::Constant*> fields);
  llvm::Constant* Word(std::uint32_t value);
  llvm::Constant* Wide(std::uint64_t value);
  llvm::Constant* Identity(FunctionIdentity identity);
  llvm::Constant* Identities(std::span<const DeclarationId> values);
  llvm::Constant* Declarations(std::span<const NativeDeclarationDescription> declarations);
  llvm::Constant* Text(std::string_view value);
  llvm::Constant* Table(llvm::Type* element, llvm::ArrayRef<llvm::Constant*> values);
  llvm::Constant* Numbers(std::span<const std::uint32_t> values);
  llvm::Constant* Texts(std::span<const NativeText> values);
  llvm::Constant* Type(const NativeTypeDescription& type);
  llvm::Constant* Classes(std::span<const NativeClassDescription> classes);
  llvm::Constant* Arrays(std::span<const NativeArrayDescription> arrays);
  llvm::Constant* Collections(std::span<const NativeCollectionDescription> collections);
  llvm::Constant* Signatures(std::span<const NativeSignatureDescription> signatures);
  llvm::Constant* Functions(std::span<const NativeFunctionDescription> functions,
                            std::span<llvm::Function* const> entries);
  llvm::Constant* Dependencies(const NativeFunctionDependencies* dependencies);
  llvm::Constant* DependencyBlocks(std::span<const NativeDependencyBlock> descriptions);
  llvm::Constant* Members(std::span<const NativeMemberDescription> members);
  llvm::Constant* JSONPlans(std::span<const NativeJSONDecodePlan> plans);

  llvm::Module& module_;
  llvm::LLVMContext& context_;
  llvm::Type* word_;
  llvm::Type* wide_;
  llvm::Type* pointer_;
  llvm::StructType* text_;
  llvm::StructType* table_;
  llvm::StructType* type_;
  llvm::StructType* class_;
  llvm::StructType* field_;
  llvm::StructType* array_;
  llvm::StructType* collection_;
  llvm::StructType* signature_parameter_;
  llvm::StructType* signature_;
  llvm::StructType* call_parameter_;
  llvm::StructType* location_;
  llvm::StructType* function_;
  llvm::StructType* dependencies_;
  llvm::StructType* dependency_block_;
  llvm::StructType* binding_;
  llvm::StructType* construction_;
  llvm::StructType* function_identity_;
  llvm::StructType* declaration_;
  llvm::StructType* member_;
  llvm::StructType* image_;
  llvm::StructType* json_parameter_;
  llvm::StructType* json_plan_;
  bool has_compatible_layout_ = true;
};

ImageEmitter::ImageEmitter(llvm::Module& module)
    : module_(module),
      context_(module.getContext()),
      word_(llvm::Type::getInt32Ty(context_)),
      wide_(llvm::Type::getInt64Ty(context_)),
      pointer_(llvm::PointerType::getUnqual(context_)) {
  llvm::Type* count = llvm::Type::getInt64Ty(context_);
  text_ =
      RecordType<NativeText>("vela.text", std::array{pointer_, count},
                             std::array{offsetof(NativeText, data), offsetof(NativeText, size)});
  table_ = RecordType<NativeTable<std::uint32_t>>("vela.table", std::array{pointer_, count},
                                                  std::array{
                                                      offsetof(NativeTable<std::uint32_t>, data),
                                                      offsetof(NativeTable<std::uint32_t>, size),
                                                  });
  type_ = RecordType<NativeTypeDescription>("vela.type", std::array{word_, word_, word_},
                                            std::array{
                                                offsetof(NativeTypeDescription, kind),
                                                offsetof(NativeTypeDescription, nullable),
                                                offsetof(NativeTypeDescription, index),
                                            });
  field_ = RecordType<NativeFieldDescription>(
      "vela.field", std::to_array<llvm::Type*>({wide_, word_, word_, word_}),
      std::array{
          offsetof(NativeFieldDescription, declaration),
          offsetof(NativeFieldDescription, type),
          offsetof(NativeFieldDescription, flags),
          offsetof(NativeFieldDescription, initializer),
      });
  class_ =
      RecordType<NativeClassDescription>("vela.class",
                                         std::to_array<llvm::Type*>({
                                             text_,
                                             text_,
                                             text_,
                                             table_,
                                             table_,
                                             table_,
                                             word_,
                                             word_,
                                             word_,
                                             word_,
                                             word_,
                                             wide_,
                                             table_,
                                             wide_,
                                         }),
                                         std::array{
                                             offsetof(NativeClassDescription, name),
                                             offsetof(NativeClassDescription, native_key),
                                             offsetof(NativeClassDescription, native_contract),
                                             offsetof(NativeClassDescription, reference_fields),
                                             offsetof(NativeClassDescription, interfaces),
                                             offsetof(NativeClassDescription, dispatch_targets),
                                             offsetof(NativeClassDescription, field_count),
                                             offsetof(NativeClassDescription, kind),
                                             offsetof(NativeClassDescription, callable_function),
                                             offsetof(NativeClassDescription, callable_mode),
                                             offsetof(NativeClassDescription, callable_type),
                                             offsetof(NativeClassDescription, declaration),
                                             offsetof(NativeClassDescription, fields),
                                             offsetof(NativeClassDescription, base_class),
                                         });
  array_ =
      RecordType<NativeArrayDescription>("vela.array", std::to_array<llvm::Type*>({text_, type_}),
                                         std::array{
                                             offsetof(NativeArrayDescription, name),
                                             offsetof(NativeArrayDescription, element),
                                         });
  collection_ = RecordType<NativeCollectionDescription>(
      "vela.collection", std::to_array<llvm::Type*>({text_, type_, type_, word_}),
      std::array{
          offsetof(NativeCollectionDescription, name),
          offsetof(NativeCollectionDescription, key),
          offsetof(NativeCollectionDescription, value),
          offsetof(NativeCollectionDescription, has_value),
      });
  signature_parameter_ = RecordType<NativeSignatureParameter>(
      "vela.signature.parameter", std::to_array<llvm::Type*>({type_, word_}),
      std::array{
          offsetof(NativeSignatureParameter, type),
          offsetof(NativeSignatureParameter, flags),
      });
  signature_ = RecordType<NativeSignatureDescription>(
      "vela.signature", std::to_array<llvm::Type*>({text_, table_, type_, word_}),
      std::array{
          offsetof(NativeSignatureDescription, name),
          offsetof(NativeSignatureDescription, parameters),
          offsetof(NativeSignatureDescription, result),
          offsetof(NativeSignatureDescription, has_result),
      });
  call_parameter_ = RecordType<NativeCallParameter>(
      "vela.call.parameter", std::to_array<llvm::Type*>({text_, word_, word_}),
      std::array{
          offsetof(NativeCallParameter, name),
          offsetof(NativeCallParameter, type),
          offsetof(NativeCallParameter, flags),
      });
  location_ = RecordType<SourceLocation>("vela.location", std::array{word_, word_, word_},
                                         std::array{
                                             offsetof(SourceLocation, line),
                                             offsetof(SourceLocation, column),
                                             offsetof(SourceLocation, source_id),
                                         });
  function_identity_ =
      RecordType<FunctionIdentity>("vela.function.identity", std::array{wide_, wide_, word_},
                                   std::array{
                                       offsetof(FunctionIdentity, declaration),
                                       offsetof(FunctionIdentity, revision),
                                       offsetof(FunctionIdentity, local),
                                   });
  declaration_ = RecordType<NativeDeclarationDescription>(
      "vela.declaration",
      std::to_array<llvm::Type*>({wide_, wide_, word_, text_, table_, table_, text_, wide_}),
      std::array{
          offsetof(NativeDeclarationDescription, identity),
          offsetof(NativeDeclarationDescription, owner),
          offsetof(NativeDeclarationDescription, kind),
          offsetof(NativeDeclarationDescription, name),
          offsetof(NativeDeclarationDescription, type_parameters),
          offsetof(NativeDeclarationDescription, enum_members),
          offsetof(NativeDeclarationDescription, generic_signature),
          offsetof(NativeDeclarationDescription, definition),
      });
  function_ = RecordType<NativeFunctionDescription>(
      "vela.function",
      std::to_array<llvm::Type*>({
          text_,
          text_,
          table_,
          table_,
          pointer_,
          pointer_,
          word_,
          word_,
          word_,
          word_,
          word_,
          function_identity_,
          pointer_,
      }),
      std::array{
          offsetof(NativeFunctionDescription, name),
          offsetof(NativeFunctionDescription, native_symbol),
          offsetof(NativeFunctionDescription, parameters),
          offsetof(NativeFunctionDescription, locations),
          offsetof(NativeFunctionDescription, entry),
          offsetof(NativeFunctionDescription, typed_entry),
          offsetof(NativeFunctionDescription, parameter_count),
          offsetof(NativeFunctionDescription, result_type),
          offsetof(NativeFunctionDescription, hidden_parameters),
          offsetof(NativeFunctionDescription, receiver_type),
          offsetof(NativeFunctionDescription, flags),
          offsetof(NativeFunctionDescription, identity),
          offsetof(NativeFunctionDescription, dependencies),
      });
  construction_ = RecordType<NativeConstructionDescription>(
      "vela.construction", std::to_array<llvm::Type*>({wide_, table_}),
      std::array{
          offsetof(NativeConstructionDescription, type),
          offsetof(NativeConstructionDescription, supplied),
      });
  binding_ = RecordType<NativeBindingDependency>("vela.binding", std::array{wide_, wide_},
                                                 std::array{
                                                     offsetof(NativeBindingDependency, target),
                                                     offsetof(NativeBindingDependency, receiver),
                                                 });
  dependency_block_ = RecordType<NativeDependencyBlock>(
      "vela.dependency.block", std::to_array<llvm::Type*>({word_, word_, table_}),
      std::array{
          offsetof(NativeDependencyBlock, begin),
          offsetof(NativeDependencyBlock, end),
          offsetof(NativeDependencyBlock, successors),
      });
  dependencies_ = RecordType<NativeFunctionDependencies>(
      "vela.dependencies",
      std::to_array<llvm::Type*>(
          {table_, table_, table_, table_, wide_, table_, table_, table_, table_}),
      std::array{
          offsetof(NativeFunctionDependencies, fields),
          offsetof(NativeFunctionDependencies, globals),
          offsetof(NativeFunctionDependencies, versions),
          offsetof(NativeFunctionDependencies, constructions),
          offsetof(NativeFunctionDependencies, lexical_class),
          offsetof(NativeFunctionDependencies, bindings),
          offsetof(NativeFunctionDependencies, types),
          offsetof(NativeFunctionDependencies, positions),
          offsetof(NativeFunctionDependencies, blocks),
      });
  member_ = RecordType<NativeMemberDescription>(
      "vela.member", std::to_array<llvm::Type*>({text_, word_, word_, word_, word_, word_}),
      std::array{
          offsetof(NativeMemberDescription, name),
          offsetof(NativeMemberDescription, receiver_type),
          offsetof(NativeMemberDescription, target),
          offsetof(NativeMemberDescription, type),
          offsetof(NativeMemberDescription, setter),
          offsetof(NativeMemberDescription, kind),
      });
  json_parameter_ = RecordType<NativeJSONDecodeParameter>(
      "vela.json.parameter", std::to_array<llvm::Type*>({text_, word_, word_}),
      std::array{
          offsetof(NativeJSONDecodeParameter, name),
          offsetof(NativeJSONDecodeParameter, node),
          offsetof(NativeJSONDecodeParameter, optional),
      });
  json_plan_ = RecordType<NativeJSONDecodePlan>(
      "vela.json.plan",
      std::to_array<llvm::Type*>(
          {text_, table_, llvm::Type::getInt8Ty(context_), word_, word_, word_}),
      std::array{
          offsetof(NativeJSONDecodePlan, name),
          offsetof(NativeJSONDecodePlan, parameters),
          offsetof(NativeJSONDecodePlan, kind),
          offsetof(NativeJSONDecodePlan, type),
          offsetof(NativeJSONDecodePlan, element),
          offsetof(NativeJSONDecodePlan, constructor),
      });
  image_ = RecordType<NativeImage>(
      "vela.image",
      std::to_array<llvm::Type*>({
          word_,  word_,  text_,  text_,  table_, table_, table_, table_, table_,
          table_, table_, table_, table_, table_, table_, table_, table_, wide_,
          table_, table_, table_, table_, table_, word_,  table_, table_,
      }),
      std::array{
          offsetof(NativeImage, abi_version),
          offsetof(NativeImage, record_bytes),
          offsetof(NativeImage, abi_identity),
          offsetof(NativeImage, target),
          offsetof(NativeImage, interfaces),
          offsetof(NativeImage, classes),
          offsetof(NativeImage, arrays),
          offsetof(NativeImage, collections),
          offsetof(NativeImage, signatures),
          offsetof(NativeImage, types),
          offsetof(NativeImage, array_types),
          offsetof(NativeImage, collection_types),
          offsetof(NativeImage, strings),
          offsetof(NativeImage, source_paths),
          offsetof(NativeImage, functions),
          offsetof(NativeImage, members),
          offsetof(NativeImage, global_initializers),
          offsetof(NativeImage, revision),
          offsetof(NativeImage, source_texts),
          offsetof(NativeImage, declarations),
          offsetof(NativeImage, interface_declarations),
          offsetof(NativeImage, global_declarations),
          offsetof(NativeImage, selector_declarations),
          offsetof(NativeImage, native_mode),
          offsetof(NativeImage, global_flags),
          offsetof(NativeImage, json_decode_plans),
      });
}

llvm::Constant* ImageEmitter::Record(llvm::StructType* type,
                                     std::initializer_list<llvm::Constant*> fields) {
  return llvm::ConstantStruct::get(type, llvm::ArrayRef(fields));
}

llvm::Constant* ImageEmitter::Word(std::uint32_t value) {
  return llvm::ConstantInt::get(word_, value);
}

llvm::Constant* ImageEmitter::Wide(std::uint64_t value) {
  return llvm::ConstantInt::get(wide_, value);
}

llvm::Constant* ImageEmitter::Identity(FunctionIdentity identity) {
  return Record(function_identity_,
                {Wide(identity.declaration.value), Wide(identity.revision), Word(identity.local)});
}

llvm::Constant* ImageEmitter::Identities(std::span<const DeclarationId> values) {
  std::vector<llvm::Constant*> constants;
  constants.reserve(values.size());
  for (const auto identity : values) {
    constants.push_back(Wide(identity.value));
  }
  return Table(wide_, constants);
}

llvm::Constant* ImageEmitter::Declarations(
    std::span<const NativeDeclarationDescription> declarations) {
  std::vector<llvm::Constant*> constants;
  constants.reserve(declarations.size());
  for (const auto& declaration : declarations) {
    constants.push_back(Record(declaration_, {
                                                 Wide(declaration.identity.value),
                                                 Wide(declaration.owner.value),
                                                 Word(static_cast<std::uint32_t>(declaration.kind)),
                                                 Text(declaration.name.text()),
                                                 Texts(declaration.type_parameters.values()),
                                                 Texts(declaration.enum_members.values()),
                                                 Text(declaration.generic_signature.text()),
                                                 Wide(declaration.definition.value),
                                             }));
  }
  return Table(declaration_, constants);
}

llvm::Constant* ImageEmitter::Text(std::string_view value) {
  auto* bytes = llvm::ConstantDataArray::getString(context_, value, false);
  auto* storage = new llvm::GlobalVariable(module_, bytes->getType(), true,
                                           llvm::GlobalValue::PrivateLinkage, bytes, "vela.utf8");
  storage->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  return Record(text_,
                {storage, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), value.size())});
}

llvm::Constant* ImageEmitter::Table(llvm::Type* element, llvm::ArrayRef<llvm::Constant*> values) {
  return EmitConstantTable(module_, table_, element, values, "vela.table");
}

llvm::Constant* ImageEmitter::Numbers(std::span<const std::uint32_t> values) {
  std::vector<llvm::Constant*> constants;
  constants.reserve(values.size());
  for (const auto value : values) {
    constants.push_back(Word(value));
  }
  return Table(word_, constants);
}

llvm::Constant* ImageEmitter::Texts(std::span<const NativeText> values) {
  std::vector<llvm::Constant*> constants;
  constants.reserve(values.size());
  for (const auto& value : values) {
    constants.push_back(Text(value.text()));
  }
  return Table(text_, constants);
}

llvm::Constant* ImageEmitter::Type(const NativeTypeDescription& type) {
  return Record(type_, {
                           Word(static_cast<std::uint32_t>(type.kind)),
                           Word(static_cast<std::uint32_t>(type.nullable)),
                           Word(type.index),
                       });
}

llvm::Constant* ImageEmitter::Classes(std::span<const NativeClassDescription> classes) {
  std::vector<llvm::Constant*> constants;
  for (const auto& type : classes) {
    std::vector<llvm::Constant*> fields;
    fields.reserve(type.fields.size);
    for (const auto& field : type.fields.values()) {
      fields.push_back(Record(field_, {
                                          Wide(field.declaration.value),
                                          Word(field.type),
                                          Word(field.flags),
                                          Word(field.initializer),
                                      }));
    }
    constants.push_back(Record(class_, {
                                           Text(type.name.text()),
                                           Text(type.native_key.text()),
                                           Text(type.native_contract.text()),
                                           Numbers(type.reference_fields.values()),
                                           Numbers(type.interfaces.values()),
                                           Numbers(type.dispatch_targets.values()),
                                           Word(type.field_count),
                                           Word(static_cast<std::uint32_t>(type.kind)),
                                           Word(type.callable_function),
                                           Word(type.callable_mode),
                                           Word(type.callable_type),
                                           Wide(type.declaration.value),
                                           Table(field_, fields),
                                           Wide(type.base_class.value),
                                       }));
  }
  return Table(class_, constants);
}

llvm::Constant* ImageEmitter::Arrays(std::span<const NativeArrayDescription> arrays) {
  std::vector<llvm::Constant*> constants;
  for (const auto& type : arrays) {
    constants.push_back(Record(array_, {Text(type.name.text()), Type(type.element)}));
  }
  return Table(array_, constants);
}

llvm::Constant* ImageEmitter::Collections(
    std::span<const NativeCollectionDescription> collections) {
  std::vector<llvm::Constant*> constants;
  for (const auto& type : collections) {
    constants.push_back(Record(collection_, {
                                                Text(type.name.text()),
                                                Type(type.key),
                                                Type(type.value),
                                                Word(type.has_value),
                                            }));
  }
  return Table(collection_, constants);
}

llvm::Constant* ImageEmitter::Signatures(std::span<const NativeSignatureDescription> signatures) {
  std::vector<llvm::Constant*> constants;
  for (const auto& type : signatures) {
    std::vector<llvm::Constant*> parameters;
    for (const auto& parameter : type.parameters.values()) {
      parameters.push_back(
          Record(signature_parameter_, {Type(parameter.type), Word(parameter.flags)}));
    }
    constants.push_back(Record(signature_, {
                                               Text(type.name.text()),
                                               Table(signature_parameter_, parameters),
                                               Type(type.result),
                                               Word(type.has_result),
                                           }));
  }
  return Table(signature_, constants);
}

llvm::Constant* ImageEmitter::Functions(std::span<const NativeFunctionDescription> functions,
                                        std::span<llvm::Function* const> entries) {
  std::vector<llvm::Constant*> constants;
  for (std::size_t index = 0; index < functions.size(); ++index) {
    const auto& function = functions.subspan(index).front();
    auto* entry = entries.subspan(index).front();
    std::vector<llvm::Constant*> parameters;
    for (const auto& parameter : function.parameters.values()) {
      parameters.push_back(Record(call_parameter_, {
                                                       Text(parameter.name.text()),
                                                       Word(parameter.type),
                                                       Word(parameter.flags),
                                                   }));
    }
    std::vector<llvm::Constant*> locations;
    for (const auto& location : function.locations.values()) {
      locations.push_back(Record(
          location_, {Word(location.line), Word(location.column), Word(location.source_id)}));
    }
    constants.push_back(Record(function_, {
                                              Text(function.name.text()),
                                              Text(function.native_symbol.text()),
                                              Table(call_parameter_, parameters),
                                              Table(location_, locations),
                                              entry,
                                              llvm::Constant::getNullValue(pointer_),
                                              Word(function.parameter_count),
                                              Word(function.result_type),
                                              Word(function.hidden_parameters),
                                              Word(function.receiver_type),
                                              Word(function.flags),
                                              Identity(function.identity),
                                              Dependencies(function.dependencies),
                                          }));
  }
  return Table(function_, constants);
}

llvm::Constant* ImageEmitter::DependencyBlocks(
    std::span<const NativeDependencyBlock> descriptions) {
  std::vector<llvm::Constant*> blocks;
  blocks.reserve(descriptions.size());
  for (const auto& block : descriptions) {
    blocks.push_back(Record(dependency_block_, {
                                                   Word(static_cast<std::uint32_t>(block.begin)),
                                                   Word(static_cast<std::uint32_t>(block.end)),
                                                   Numbers(block.successors.values()),
                                               }));
  }
  return Table(dependency_block_, blocks);
}

llvm::Constant* ImageEmitter::Dependencies(const NativeFunctionDependencies* dependencies) {
  if (dependencies == nullptr) {
    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context_));
  }
  std::vector<llvm::Constant*> constructions;
  std::vector<llvm::Constant*> bindings;
  for (const auto& binding : dependencies->bindings.values()) {
    bindings.push_back(
        Record(binding_, {Wide(binding.target.value), Wide(binding.receiver.value)}));
  }
  for (const auto& construction : dependencies->constructions.values()) {
    constructions.push_back(Record(construction_, {
                                                      Wide(construction.type.value),
                                                      Identities(construction.supplied.values()),
                                                  }));
  }
  auto* record = Record(dependencies_, {
                                           Identities(dependencies->fields.values()),
                                           Identities(dependencies->globals.values()),
                                           Numbers(dependencies->versions.values()),
                                           Table(construction_, constructions),
                                           Wide(dependencies->lexical_class.value),
                                           Table(binding_, bindings),
                                           Identities(dependencies->types.values()),
                                           Numbers(dependencies->positions.values()),
                                           DependencyBlocks(dependencies->blocks.values()),
                                       });
  auto* storage = new llvm::GlobalVariable(
      module_, dependencies_, true, llvm::GlobalValue::PrivateLinkage, record, "vela.dependencies");
  return storage;
}

llvm::Constant* ImageEmitter::Members(std::span<const NativeMemberDescription> members) {
  std::vector<llvm::Constant*> constants;
  for (const auto& member : members) {
    constants.push_back(Record(member_, {
                                            Text(member.name.text()),
                                            Word(member.receiver_type),
                                            Word(member.target),
                                            Word(member.type),
                                            Word(member.setter),
                                            Word(static_cast<std::uint32_t>(member.kind)),
                                        }));
  }
  return Table(member_, constants);
}

std::expected<llvm::Constant*, std::string> ImageEmitter::Emit(
    const NativeImage& image,
    std::span<llvm::Function* const> entries) {
  if (!has_compatible_layout_) {
    return std::unexpected(
        "LLVM target data layout does not match the native runtime descriptor ABI");
  }
  std::vector<llvm::Constant*> types;
  types.reserve(image.types.size);
  for (const auto& type : image.types.values()) {
    types.push_back(Type(type));
  }
  auto* description = Record(image_, {
                                         Word(image.abi_version),
                                         Word(image.record_bytes),
                                         Text(image.abi_identity.text()),
                                         Text(image.target.text()),
                                         Texts(image.interfaces.values()),
                                         Classes(image.classes.values()),
                                         Arrays(image.arrays.values()),
                                         Collections(image.collections.values()),
                                         Signatures(image.signatures.values()),
                                         Table(type_, types),
                                         Numbers(image.array_types.values()),
                                         Numbers(image.collection_types.values()),
                                         Texts(image.strings.values()),
                                         Texts(image.source_paths.values()),
                                         Functions(image.functions.values(), entries),
                                         Members(image.members.values()),
                                         Numbers(image.global_initializers.values()),
                                         Wide(image.revision),
                                         Texts(image.source_texts.values()),
                                         Declarations(image.declarations.values()),
                                         Identities(image.interface_declarations.values()),
                                         Identities(image.global_declarations.values()),
                                         Identities(image.selector_declarations.values()),
                                         Word(image.native_mode),
                                         Numbers(image.global_flags.values()),
                                         JSONPlans(image.json_decode_plans.values()),
                                     });
  auto* storage = new llvm::GlobalVariable(module_, image_, true, llvm::GlobalValue::PrivateLinkage,
                                           description, "vela.image");
  return storage;
}

llvm::Constant* ImageEmitter::JSONPlans(std::span<const NativeJSONDecodePlan> plans) {
  std::vector<llvm::Constant*> nodes;
  for (const auto& plan : plans) {
    std::vector<llvm::Constant*> parameters;
    for (const auto& parameter : plan.parameters.values()) {
      parameters.push_back(
          Record(json_parameter_,
                 {Text(parameter.name.text()), Word(parameter.node), Word(parameter.optional)}));
    }
    nodes.push_back(
        Record(json_plan_, {
                               Text(plan.name.text()),
                               Table(json_parameter_, parameters),
                               llvm::ConstantInt::get(llvm::Type::getInt8Ty(context_),
                                                      static_cast<std::uint8_t>(plan.kind)),
                               Word(plan.type),
                               Word(plan.element),
                               Word(plan.constructor),
                           }));
  }
  return Table(json_plan_, nodes);
}

}  // namespace

std::expected<EmittedNativeImage, std::string> EmitNativeImage(
    llvm::Module& module,
    const NativeImage& description,
    std::span<llvm::Function* const> entries,
    std::string_view module_name) {
  const auto image = ImageEmitter(module).Emit(description, entries);
  if (!image) {
    return std::unexpected(image.error());
  }
  auto& context = module.getContext();
  auto name = std::string(module_name) + "_image";
  auto* signature = llvm::FunctionType::get(llvm::PointerType::getUnqual(context), {}, false);
  auto* entry = llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", entry));
  builder.CreateRet(*image);
  return EmittedNativeImage{.symbol = std::move(name), .data = *image};
}

}  // namespace vela::compiler::native
