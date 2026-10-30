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

#include <absl/container/flat_hash_map.h>

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

#include "base/callable.h"
#include "base/declaration.h"
#include "base/layout_description.h"
#include "base/member.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "base/source_location.h"
#include "compiler/frontend/ast.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "runtime/native_abi.h"
#include "runtime/native_build_identity.h"
#include "runtime/native_image.h"

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
using runtime::NativeMemberDescription;
using runtime::NativeSignatureDescription;
using runtime::NativeSignatureParameter;
using runtime::NativeTable;
using runtime::NativeText;
using runtime::NativeTypeDescription;

std::uint32_t FunctionFlags(const ir::Function& function, std::uint8_t exported) {
  std::uint32_t flags = function.is_async ? runtime::kNativeAsyncFunction : 0;
  if (function.is_generated_constructor) {
    flags |= runtime::kNativeGeneratedConstructor;
  }
  if (function.visibility == frontend::Visibility::Private) {
    flags |= runtime::kNativePrivateFunction;
  }
  if (exported != 0) {
    flags |= runtime::kNativeExportedFunction;
  }
  if (function.return_type.kind == frontend::TypeDescriptor::Kind::Enum) {
    flags |= runtime::kNativeEnumResult;
  }
  return flags;
}

class ImageEmitter final {
 public:
  explicit ImageEmitter(llvm::Module& module);
  ~ImageEmitter() = default;
  ImageEmitter(const ImageEmitter&) = delete;
  ImageEmitter& operator=(const ImageEmitter&) = delete;
  ImageEmitter(ImageEmitter&&) = delete;
  ImageEmitter& operator=(ImageEmitter&&) = delete;
  std::expected<llvm::Constant*, std::string> Emit(const ir::Program& program,
                                                   std::span<llvm::Function* const> entries,
                                                   std::span<const std::uint8_t> exports);

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
  llvm::Constant* Declarations(std::span<const ir::Declaration> declarations);
  llvm::Constant* Text(std::string_view value);
  llvm::Constant* Table(llvm::Type* element, llvm::ArrayRef<llvm::Constant*> values);
  llvm::Constant* Numbers(std::span<const std::uint32_t> values);
  llvm::Constant* Texts(std::span<const std::string> values);
  llvm::Constant* Type(const layout::Type& type);
  llvm::Constant* Classes(const ir::Program& program);
  llvm::Constant* Arrays(std::span<const layout::Array> arrays);
  llvm::Constant* Collections(std::span<const layout::Collection> collections);
  llvm::Constant* Signatures(std::span<const layout::Signature> signatures);
  llvm::Constant* Functions(const ir::Program& program,
                            std::span<llvm::Function* const> entries,
                            std::span<const std::uint8_t> exports);
  llvm::Constant* Dependencies(const ir::Program& program, const ir::Function& function);
  llvm::Constant* DependencyBlocks(const ir::Function& function);
  [[nodiscard]] DeclarationId LexicalClass(DeclarationId owner) const;
  llvm::Constant* Members(std::span<const MemberDefinition> members);

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
  bool has_compatible_layout_ = true;
  absl::flat_hash_map<DeclarationId, const ir::Declaration*> declarations_;
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
                                             table_,
                                             table_,
                                             table_,
                                             word_,
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
                                             offsetof(NativeClassDescription, reference_fields),
                                             offsetof(NativeClassDescription, interfaces),
                                             offsetof(NativeClassDescription, dispatch_targets),
                                             offsetof(NativeClassDescription, field_count),
                                             offsetof(NativeClassDescription, kind),
                                             offsetof(NativeClassDescription, callable_function),
                                             offsetof(NativeClassDescription, callable_mode),
                                             offsetof(NativeClassDescription, callable_type),
                                             offsetof(NativeClassDescription, expected_type),
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
      std::to_array<llvm::Type*>({wide_, wide_, word_, text_, table_, table_, wide_}),
      std::array{
          offsetof(NativeDeclarationDescription, identity),
          offsetof(NativeDeclarationDescription, owner),
          offsetof(NativeDeclarationDescription, kind),
          offsetof(NativeDeclarationDescription, name),
          offsetof(NativeDeclarationDescription, type_parameters),
          offsetof(NativeDeclarationDescription, enum_members),
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
          offsetof(NativeFunctionDescription, parameter_count),
          offsetof(NativeFunctionDescription, result_type),
          offsetof(NativeFunctionDescription, hidden_parameters),
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
  image_ = RecordType<NativeImage>(
      "vela.image",
      std::to_array<llvm::Type*>({
          word_,  word_,  text_,  text_,  table_, table_, table_, table_, table_,
          table_, table_, table_, table_, table_, table_, table_, table_, wide_,
          table_, table_, table_, table_, table_, word_,  table_,
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

llvm::Constant* ImageEmitter::Declarations(std::span<const ir::Declaration> declarations) {
  std::vector<llvm::Constant*> constants;
  constants.reserve(declarations.size());
  for (const auto& declaration : declarations) {
    constants.push_back(Record(declaration_, {
                                                 Wide(declaration.identity.value),
                                                 Wide(declaration.owner.value),
                                                 Word(static_cast<std::uint32_t>(declaration.kind)),
                                                 Text(declaration.name),
                                                 Texts(declaration.type_parameters),
                                                 Texts(declaration.enum_members),
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
  llvm::Constant* storage = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context_));
  if (!values.empty()) {
    auto* array = llvm::ConstantArray::get(llvm::ArrayType::get(element, values.size()), values);
    auto* global = new llvm::GlobalVariable(module_, array->getType(), true,
                                            llvm::GlobalValue::PrivateLinkage, array, "vela.table");
    storage = global;
  }
  return Record(table_,
                {storage, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), values.size())});
}

llvm::Constant* ImageEmitter::Numbers(std::span<const std::uint32_t> values) {
  std::vector<llvm::Constant*> constants;
  constants.reserve(values.size());
  for (const auto value : values) {
    constants.push_back(Word(value));
  }
  return Table(word_, constants);
}

llvm::Constant* ImageEmitter::Texts(std::span<const std::string> values) {
  std::vector<llvm::Constant*> constants;
  constants.reserve(values.size());
  for (const auto& value : values) {
    constants.push_back(Text(value));
  }
  return Table(text_, constants);
}

llvm::Constant* ImageEmitter::Type(const layout::Type& type) {
  return Record(type_, {
                           Word(static_cast<std::uint32_t>(type.kind)),
                           Word(static_cast<std::uint32_t>(type.nullable)),
                           Word(type.index),
                       });
}

llvm::Constant* ImageEmitter::Classes(const ir::Program& program) {
  std::vector<llvm::Constant*> constants;
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    const auto& type = program.layouts.classes.at(index);
    const auto& source = program.classes.at(index);
    std::vector<llvm::Constant*> fields;
    fields.reserve(source.fields.size());
    for (const auto& field : source.fields) {
      fields.push_back(Record(field_, {
                                          Wide(field.declaration.value),
                                          Word(field.type),
                                          Word(field.visibility == frontend::Visibility::Private
                                                   ? runtime::kNativePrivateField
                                                   : 0),
                                          Word(field.initializer),
                                      }));
    }
    constants.push_back(Record(
        class_, {
                    Text(type.name),
                    Numbers(type.reference_fields),
                    Numbers(type.interfaces),
                    Numbers(type.dispatch_targets),
                    Word(type.field_count),
                    Word(static_cast<std::uint32_t>(type.kind)),
                    Word(type.callable ? type.callable->function : runtime::kNoNativeIndex),
                    Word(type.callable ? static_cast<std::uint32_t>(type.callable->mode) : 0),
                    Word(type.callable_type.value_or(runtime::kNoNativeIndex)),
                    Word(type.expected_type.value_or(runtime::kNoNativeIndex)),
                    Wide(source.declaration.value),
                    Table(field_, fields),
                    Wide(source.base_class.value),
                }));
  }
  return Table(class_, constants);
}

llvm::Constant* ImageEmitter::Arrays(std::span<const layout::Array> arrays) {
  std::vector<llvm::Constant*> constants;
  for (const auto& type : arrays) {
    constants.push_back(Record(array_, {Text(type.name), Type(type.element)}));
  }
  return Table(array_, constants);
}

llvm::Constant* ImageEmitter::Collections(std::span<const layout::Collection> collections) {
  std::vector<llvm::Constant*> constants;
  for (const auto& type : collections) {
    constants.push_back(
        Record(collection_, {
                                Text(type.name),
                                Type(type.key),
                                Type(type.value.value_or(layout::Type())),
                                Word(static_cast<std::uint32_t>(type.value.has_value())),
                            }));
  }
  return Table(collection_, constants);
}

llvm::Constant* ImageEmitter::Signatures(std::span<const layout::Signature> signatures) {
  std::vector<llvm::Constant*> constants;
  for (const auto& type : signatures) {
    std::vector<llvm::Constant*> parameters;
    for (const auto& parameter : type.parameters) {
      const auto flags = (parameter.optional ? runtime::kNativeOptionalParameter : 0U) |
                         (parameter.rest ? runtime::kNativeRestParameter : 0U);
      parameters.push_back(Record(signature_parameter_, {Type(parameter.type), Word(flags)}));
    }
    constants.push_back(
        Record(signature_, {
                               Text(type.name),
                               Table(signature_parameter_, parameters),
                               Type(type.result.value_or(layout::Type())),
                               Word(static_cast<std::uint32_t>(type.result.has_value())),
                           }));
  }
  return Table(signature_, constants);
}

llvm::Constant* ImageEmitter::Functions(const ir::Program& program,
                                        std::span<llvm::Function* const> entries,
                                        std::span<const std::uint8_t> exports) {
  std::vector<llvm::Constant*> constants;
  const auto functions = std::span(program.functions);
  for (std::size_t index = 0; index < functions.size(); ++index) {
    const auto& function = functions.subspan(index).front();
    auto* entry = entries.subspan(index).front();
    std::vector<llvm::Constant*> parameters;
    std::uint32_t result = runtime::kNoNativeIndex;
    std::uint32_t hidden = 0;
    auto flags = FunctionFlags(function, exports.subspan(index).front());
    if (function.signature) {
      const auto& signature = *function.signature;
      flags |= runtime::kNativeTypedFunction;
      hidden = signature.hidden_parameters;
      result = signature.result.value_or(runtime::kNoNativeIndex);
      for (const auto& parameter : signature.parameters) {
        const auto parameter_flags = (parameter.optional ? runtime::kNativeOptionalParameter : 0U) |
                                     (parameter.rest ? runtime::kNativeRestParameter : 0U);
        parameters.push_back(Record(
            call_parameter_, {Text(parameter.name), Word(parameter.type), Word(parameter_flags)}));
      }
    }
    std::vector<llvm::Constant*> locations;
    if (!function.native_symbol.empty()) {
      const auto location = function.location;
      locations.push_back(Record(location_, {
                                                Word(location.line),
                                                Word(location.column),
                                                Word(location.source_id),
                                            }));
    }
    for (const auto& instruction : function.instructions) {
      const auto& location = instruction.location;
      locations.push_back(Record(
          location_, {Word(location.line), Word(location.column), Word(location.source_id)}));
    }
    constants.push_back(Record(function_, {
                                              Text(function.name),
                                              Text(function.native_symbol),
                                              Table(call_parameter_, parameters),
                                              Table(location_, locations),
                                              entry,
                                              Word(function.parameter_count),
                                              Word(result),
                                              Word(hidden),
                                              Word(flags),
                                              Identity(function.identity),
                                              Dependencies(program, function),
                                          }));
  }
  return Table(function_, constants);
}

llvm::Constant* ImageEmitter::DependencyBlocks(const ir::Function& function) {
  const auto graph = ir::BuildFlowGraph(function);
  std::vector<llvm::Constant*> blocks;
  blocks.reserve(graph.blocks.size());
  for (const auto& block : graph.blocks) {
    std::vector<std::uint32_t> successors;
    successors.reserve(block.successors.size());
    for (const auto successor : block.successors) {
      successors.push_back(static_cast<std::uint32_t>(successor));
    }
    blocks.push_back(Record(dependency_block_, {
                                                   Word(static_cast<std::uint32_t>(block.begin)),
                                                   Word(static_cast<std::uint32_t>(block.end)),
                                                   Numbers(successors),
                                               }));
  }
  return Table(dependency_block_, blocks);
}

DeclarationId ImageEmitter::LexicalClass(DeclarationId owner) const {
  auto declaration = declarations_.find(owner);
  while (declaration != declarations_.end()) {
    if (declaration->second->kind == DeclarationKind::Class) {
      return owner;
    }
    owner = declaration->second->owner;
    declaration = declarations_.find(owner);
  }
  return {};
}

llvm::Constant* ImageEmitter::Dependencies(const ir::Program& program,
                                           const ir::Function& function) {
  if (program.native_mode != NativeProgramMode::Reloadable) {
    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context_));
  }
  std::vector<DeclarationId> fields;
  std::vector<DeclarationId> globals;
  std::vector<std::uint32_t> versions;
  std::vector<llvm::Constant*> constructions;
  std::vector<llvm::Constant*> bindings;
  std::vector<std::uint32_t> field_positions;
  std::vector<std::uint32_t> global_positions;
  std::vector<std::uint32_t> version_positions;
  std::vector<std::uint32_t> construction_positions;
  std::vector<std::uint32_t> binding_positions;
  const auto lexical_class = LexicalClass(function.identity.declaration);
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    const auto position = static_cast<std::uint32_t>(index);
    if (instruction.field_declaration) {
      fields.push_back(*instruction.field_declaration);
      field_positions.push_back(position);
    }
    if (instruction.opcode == Opcode::GetGlobal || instruction.opcode == Opcode::SetGlobal) {
      globals.push_back(program.global_declarations.at(instruction.index));
      global_positions.push_back(position);
    }
    if (instruction.opcode == Opcode::NewInstance) {
      const auto callable = program.classes.at(instruction.index).callable;
      if (callable && callable->mode != CallableMode::InterfaceMethod &&
          program.functions.at(callable->function).identity.local != 0) {
        versions.push_back(callable->function);
        version_positions.push_back(position);
      } else if (callable && callable->mode != CallableMode::InterfaceMethod) {
        const auto target = program.functions.at(callable->function).identity.declaration;
        bindings.push_back(
            Record(binding_, {
                                 Wide(target.value),
                                 Wide(instruction.binding_receiver.value_or(DeclarationId{}).value),
                             }));
        binding_positions.push_back(position);
      }
    }
    if (instruction.opcode == Opcode::Call) {
      const auto identity = program.functions.at(instruction.index).identity;
      if (identity.local == 0) {
        bindings.push_back(
            Record(binding_, {
                                 Wide(identity.declaration.value),
                                 Wide(instruction.binding_receiver.value_or(DeclarationId{}).value),
                             }));
        binding_positions.push_back(position);
      }
    }
    if (instruction.construction && instruction.construction->generated) {
      const auto& construction = *instruction.construction;
      constructions.push_back(Record(
          construction_, {Wide(construction.type.value), Identities(construction.supplied)}));
      construction_positions.push_back(position);
    }
  }
  auto positions = std::move(field_positions);
  for (const auto* group :
       {&global_positions, &version_positions, &construction_positions, &binding_positions}) {
    positions.insert(positions.end(), group->begin(), group->end());
  }
  auto* record = Record(dependencies_, {
                                           Identities(fields),
                                           Identities(globals),
                                           Numbers(versions),
                                           Table(construction_, constructions),
                                           Wide(lexical_class.value),
                                           Table(binding_, bindings),
                                           Identities(function.type_dependencies),
                                           Numbers(positions),
                                           DependencyBlocks(function),
                                       });
  auto* storage = new llvm::GlobalVariable(
      module_, dependencies_, true, llvm::GlobalValue::PrivateLinkage, record, "vela.dependencies");
  return storage;
}

llvm::Constant* ImageEmitter::Members(std::span<const MemberDefinition> members) {
  std::vector<llvm::Constant*> constants;
  for (const auto& member : members) {
    constants.push_back(Record(member_, {
                                            Text(member.name),
                                            Word(member.receiver_type),
                                            Word(member.target),
                                            Word(member.type.value_or(runtime::kNoNativeIndex)),
                                            Word(member.setter.value_or(runtime::kNoNativeIndex)),
                                            Word(static_cast<std::uint32_t>(member.kind)),
                                        }));
  }
  return Table(member_, constants);
}

std::expected<llvm::Constant*, std::string> ImageEmitter::Emit(
    const ir::Program& program,
    std::span<llvm::Function* const> entries,
    std::span<const std::uint8_t> exports) {
  if (!has_compatible_layout_) {
    return std::unexpected(
        "LLVM target data layout does not match the native runtime descriptor ABI");
  }
  const auto& layouts = program.layouts;
  for (const auto& declaration : program.declarations) {
    declarations_.emplace(declaration.identity, &declaration);
  }
  std::vector<llvm::Constant*> types;
  types.reserve(layouts.types.size());
  for (const auto& type : layouts.types) {
    types.push_back(Type(type));
  }
  auto* description = Record(image_, {
                                         Word(runtime::kNativeAbiVersion),
                                         Word(sizeof(NativeImage)),
                                         Text(runtime::kNativeBuildIdentity),
                                         Text(module_.getTargetTriple().str()),
                                         Texts(layouts.interfaces),
                                         Classes(program),
                                         Arrays(layouts.arrays),
                                         Collections(layouts.collections),
                                         Signatures(layouts.signatures),
                                         Table(type_, types),
                                         Numbers(layouts.array_types),
                                         Numbers(layouts.collection_types),
                                         Texts(program.strings),
                                         Texts(program.source_paths),
                                         Functions(program, entries, exports),
                                         Members(program.members),
                                         Numbers(program.global_initializers),
                                         Wide(program.revision),
                                         Texts(program.source_texts),
                                         Declarations(program.declarations),
                                         Identities(program.interface_declarations),
                                         Identities(program.global_declarations),
                                         Identities(program.selector_declarations),
                                         Word(static_cast<std::uint32_t>(program.native_mode)),
                                         Numbers(program.global_flags),
                                     });
  auto* storage = new llvm::GlobalVariable(module_, image_, true, llvm::GlobalValue::PrivateLinkage,
                                           description, "vela.image");
  return storage;
}

}  // namespace

std::expected<EmittedNativeImage, std::string> EmitNativeImage(
    llvm::Module& module,
    const ir::Program& program,
    std::span<llvm::Function* const> entries,
    std::span<const std::uint8_t> exports,
    std::string_view module_name) {
  const auto image = ImageEmitter(module).Emit(program, entries, exports);
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
