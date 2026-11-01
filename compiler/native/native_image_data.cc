#include "compiler/native/native_image_data.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "base/source_location.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "runtime/native/native_image.h"
#include "runtime/native_build_identity.h"

namespace vela::compiler::native {
namespace {

runtime::NativeTypeDescription Describe(const layout::Type& type) {
  return {
      .kind = static_cast<std::uint32_t>(type.kind),
      .nullable = static_cast<std::uint32_t>(type.nullable),
      .index = type.index,
  };
}

std::optional<DeclarationId> BindingTarget(const ir::Program& program,
                                           const ir::Instruction& instruction) {
  if (instruction.opcode == Opcode::CallInterface) {
    return program.selector_declarations.at(instruction.index);
  }
  std::optional<FunctionIdentity> function;
  if (instruction.opcode == Opcode::Call) {
    function = program.functions.at(instruction.index).identity;
  } else if (instruction.opcode == Opcode::NewInstance) {
    const auto& callable = program.classes.at(instruction.index).callable;
    if (callable) {
      if (callable->mode == CallableMode::InterfaceMethod) {
        return program.selector_declarations.at(callable->function);
      }
      function = program.functions.at(callable->function).identity;
    }
  }
  return function && function->local == 0 ? std::optional(function->declaration) : std::nullopt;
}

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

}  // namespace

class NativeImageBuilder final {
 public:
  explicit NativeImageBuilder(const ir::Program& program)
      : program_(&program), data_(std::make_shared<NativeImageData>()) {
    for (const auto& declaration : program.declarations) {
      declarations_.emplace(declaration.identity, &declaration);
    }
  }

  std::shared_ptr<NativeImageData> Build(std::span<const std::uint8_t> exports);

 private:
  template <class T>
  runtime::NativeTable<T> Copy(std::span<const T> values) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (values.empty()) {
      return {};
    }
    auto* storage = data_->storage_.Allocate(values.size_bytes(), alignof(T));
    std::memcpy(storage, values.data(), values.size_bytes());
    return {.data = static_cast<const T*>(storage), .size = values.size()};
  }

  template <class T>
  runtime::NativeTable<T> Copy(const std::vector<T>& values) {
    return Copy(std::span(values));
  }

  template <class T>
  const T* Record(const T& value) {
    return Copy(std::span(&value, 1)).data;
  }

  runtime::NativeText Text(std::string_view text) {
    const auto bytes = Copy(std::span(text));
    return {.data = bytes.data, .size = bytes.size};
  }

  runtime::NativeTable<runtime::NativeText> Texts(const std::vector<std::string>& texts) {
    std::vector<runtime::NativeText> values;
    values.reserve(texts.size());
    for (const auto& text : texts) {
      values.push_back(Text(text));
    }
    return Copy(values);
  }

  runtime::NativeClassDescription Class(std::size_t index);
  runtime::NativeSignatureDescription Signature(const layout::Signature& source);
  runtime::NativeFunctionDescription Function(const ir::Function& function, std::uint8_t exported);
  const runtime::NativeFunctionDependencies* Dependencies(const ir::Function& function);
  runtime::NativeTable<runtime::NativeDependencyBlock> Blocks(const ir::Function& function);
  [[nodiscard]] DeclarationId LexicalClass(DeclarationId owner) const;

  const ir::Program* program_;
  std::shared_ptr<NativeImageData> data_;
  absl::flat_hash_map<DeclarationId, const ir::Declaration*> declarations_;
};

runtime::NativeClassDescription NativeImageBuilder::Class(std::size_t index) {
  const auto& type = program_->layouts.classes.at(index);
  const auto& source = program_->classes.at(index);
  std::vector<runtime::NativeFieldDescription> fields;
  fields.reserve(source.fields.size());
  for (const auto& field : source.fields) {
    fields.push_back({
        .declaration = field.declaration,
        .type = field.type,
        .flags =
            field.visibility == frontend::Visibility::Private ? runtime::kNativePrivateField : 0,
        .initializer = field.initializer,
    });
  }
  return {
      .name = Text(type.name),
      .reference_fields = Copy(type.reference_fields),
      .interfaces = Copy(type.interfaces),
      .dispatch_targets = Copy(type.dispatch_targets),
      .field_count = type.field_count,
      .kind = static_cast<std::uint32_t>(type.kind),
      .callable_function = type.callable ? type.callable->function : runtime::kNoNativeIndex,
      .callable_mode = type.callable ? static_cast<std::uint32_t>(type.callable->mode) : 0,
      .callable_type = type.callable_type.value_or(runtime::kNoNativeIndex),
      .declaration = source.declaration,
      .fields = Copy(fields),
      .base_class = source.base_class,
  };
}

runtime::NativeSignatureDescription NativeImageBuilder::Signature(const layout::Signature& source) {
  std::vector<runtime::NativeSignatureParameter> parameters;
  parameters.reserve(source.parameters.size());
  for (const auto& parameter : source.parameters) {
    parameters.push_back({
        .type = Describe(parameter.type),
        .flags = (parameter.optional ? runtime::kNativeOptionalParameter : 0U) |
                 (parameter.rest ? runtime::kNativeRestParameter : 0U),
    });
  }
  return {
      .name = Text(source.name),
      .parameters = Copy(parameters),
      .result = source.result ? Describe(*source.result) : runtime::NativeTypeDescription{},
      .has_result = static_cast<std::uint32_t>(source.result.has_value()),
  };
}

DeclarationId NativeImageBuilder::LexicalClass(DeclarationId owner) const {
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

runtime::NativeTable<runtime::NativeDependencyBlock> NativeImageBuilder::Blocks(
    const ir::Function& function) {
  const auto graph = ir::BuildFlowGraph(function);
  std::vector<runtime::NativeDependencyBlock> blocks;
  blocks.reserve(graph.blocks.size());
  for (const auto& block : graph.blocks) {
    std::vector<std::uint32_t> successors;
    successors.reserve(block.successors.size());
    for (const auto successor : block.successors) {
      successors.push_back(static_cast<std::uint32_t>(successor));
    }
    blocks.push_back({
        .begin = static_cast<std::uint32_t>(block.begin),
        .end = static_cast<std::uint32_t>(block.end),
        .successors = Copy(successors),
    });
  }
  return Copy(blocks);
}

const runtime::NativeFunctionDependencies* NativeImageBuilder::Dependencies(
    const ir::Function& function) {
  if (program_->native_mode != NativeProgramMode::Reloadable) {
    return nullptr;
  }
  std::vector<DeclarationId> fields;
  std::vector<DeclarationId> globals;
  std::vector<std::uint32_t> versions;
  std::vector<runtime::NativeConstructionDescription> constructions;
  std::vector<runtime::NativeBindingDependency> bindings;
  std::vector<std::uint32_t> field_positions;
  std::vector<std::uint32_t> global_positions;
  std::vector<std::uint32_t> version_positions;
  std::vector<std::uint32_t> construction_positions;
  std::vector<std::uint32_t> binding_positions;
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    const auto position = static_cast<std::uint32_t>(index);
    if (instruction.field_declaration) {
      fields.push_back(*instruction.field_declaration);
      field_positions.push_back(position);
    }
    if (instruction.opcode == Opcode::GetGlobal || instruction.opcode == Opcode::SetGlobal) {
      globals.push_back(program_->global_declarations.at(instruction.index));
      global_positions.push_back(position);
    }
    if (instruction.opcode == Opcode::NewInstance) {
      const auto callable = program_->classes.at(instruction.index).callable;
      if (callable && callable->mode != CallableMode::InterfaceMethod &&
          program_->functions.at(callable->function).identity.local != 0) {
        versions.push_back(callable->function);
        version_positions.push_back(position);
      }
    }
    if (const auto target = BindingTarget(*program_, instruction)) {
      bindings.push_back(
          {.target = *target, .receiver = instruction.binding_receiver.value_or(DeclarationId{})});
      binding_positions.push_back(position);
    }
    if (instruction.construction && instruction.construction->generated) {
      const auto& construction = *instruction.construction;
      constructions.push_back({.type = construction.type, .supplied = Copy(construction.supplied)});
      construction_positions.push_back(position);
    }
  }
  auto positions = std::move(field_positions);
  for (const auto* group :
       {&global_positions, &version_positions, &construction_positions, &binding_positions}) {
    positions.insert(positions.end(), group->begin(), group->end());
  }
  return Record(runtime::NativeFunctionDependencies{
      .fields = Copy(fields),
      .globals = Copy(globals),
      .versions = Copy(versions),
      .constructions = Copy(constructions),
      .lexical_class = LexicalClass(function.identity.declaration),
      .bindings = Copy(bindings),
      .types = Copy(function.type_dependencies),
      .positions = Copy(positions),
      .blocks = Blocks(function),
  });
}

runtime::NativeFunctionDescription NativeImageBuilder::Function(const ir::Function& function,
                                                                std::uint8_t exported) {
  runtime::NativeFunctionDescription result{
      .name = Text(function.name),
      .native_symbol = Text(function.native_symbol),
      .parameters = {},
      .locations = {},
      .parameter_count = function.parameter_count,
      .flags = FunctionFlags(function, exported),
      .identity = function.identity,
      .dependencies = Dependencies(function),
  };
  if (function.signature) {
    const auto& signature = *function.signature;
    result.flags |= runtime::kNativeTypedFunction;
    result.hidden_parameters = signature.hidden_parameters;
    result.result_type = signature.result.value_or(runtime::kNoNativeIndex);
    std::vector<runtime::NativeCallParameter> parameters;
    parameters.reserve(signature.parameters.size());
    for (const auto& parameter : signature.parameters) {
      parameters.push_back({
          .name = Text(parameter.name),
          .type = parameter.type,
          .flags = (parameter.optional ? runtime::kNativeOptionalParameter : 0U) |
                   (parameter.rest ? runtime::kNativeRestParameter : 0U),
      });
    }
    result.parameters = Copy(parameters);
  }
  std::vector<SourceLocation> locations;
  locations.reserve(function.instructions.size() + 1);
  if (!function.native_symbol.empty()) {
    locations.push_back(function.location);
  }
  for (const auto& instruction : function.instructions) {
    locations.push_back(instruction.location);
  }
  result.locations = Copy(locations);
  return result;
}

std::shared_ptr<NativeImageData> NativeImageBuilder::Build(std::span<const std::uint8_t> exports) {
  auto& image = data_->image_;
  const auto& layouts = program_->layouts;
  image.abi_identity = Text(runtime::kNativeBuildIdentity);
  image.target = Text(runtime::NativeTargetIdentity());
  image.interfaces = Texts(layouts.interfaces);
  std::vector<runtime::NativeClassDescription> classes;
  classes.reserve(program_->classes.size());
  for (std::size_t index = 0; index < program_->classes.size(); ++index) {
    classes.push_back(Class(index));
  }
  image.classes = Copy(classes);
  std::vector<runtime::NativeArrayDescription> arrays;
  arrays.reserve(layouts.arrays.size());
  for (const auto& array : layouts.arrays) {
    arrays.push_back({.name = Text(array.name), .element = Describe(array.element)});
  }
  image.arrays = Copy(arrays);
  std::vector<runtime::NativeCollectionDescription> collections;
  collections.reserve(layouts.collections.size());
  for (const auto& collection : layouts.collections) {
    collections.push_back({
        .name = Text(collection.name),
        .key = Describe(collection.key),
        .value = collection.value ? Describe(*collection.value) : runtime::NativeTypeDescription{},
        .has_value = static_cast<std::uint32_t>(collection.value.has_value()),
    });
  }
  image.collections = Copy(collections);
  std::vector<runtime::NativeSignatureDescription> signatures;
  signatures.reserve(layouts.signatures.size());
  for (const auto& signature : layouts.signatures) {
    signatures.push_back(Signature(signature));
  }
  image.signatures = Copy(signatures);
  std::vector<runtime::NativeTypeDescription> types;
  types.reserve(layouts.types.size());
  for (const auto& type : layouts.types) {
    types.push_back(Describe(type));
  }
  image.types = Copy(types);
  image.array_types = Copy(layouts.array_types);
  image.collection_types = Copy(layouts.collection_types);
  image.strings = Texts(program_->strings);
  image.source_paths = Texts(program_->source_paths);
  auto& functions = data_->functions_;
  functions.reserve(program_->functions.size());
  for (std::size_t index = 0; index < program_->functions.size(); ++index) {
    functions.push_back(Function(program_->functions.at(index), exports.subspan(index).front()));
  }
  image.functions = {.data = functions.data(), .size = functions.size()};
  std::vector<runtime::NativeMemberDescription> members;
  members.reserve(program_->members.size());
  for (const auto& member : program_->members) {
    members.push_back({
        .name = Text(member.name),
        .receiver_type = member.receiver_type,
        .target = member.target,
        .type = member.type.value_or(runtime::kNoNativeIndex),
        .setter = member.setter.value_or(runtime::kNoNativeIndex),
        .kind = static_cast<std::uint32_t>(member.kind),
    });
  }
  image.members = Copy(members);
  image.global_initializers = Copy(program_->global_initializers);
  image.revision = program_->revision;
  image.source_texts = Texts(program_->source_texts);
  std::vector<runtime::NativeDeclarationDescription> declarations;
  declarations.reserve(program_->declarations.size());
  for (const auto& declaration : program_->declarations) {
    declarations.push_back({
        .identity = declaration.identity,
        .owner = declaration.owner,
        .kind = static_cast<std::uint32_t>(declaration.kind),
        .name = Text(declaration.name),
        .type_parameters = Texts(declaration.type_parameters),
        .enum_members = Texts(declaration.enum_members),
        .generic_signature = Text(declaration.generic_signature),
        .definition = declaration.definition,
    });
  }
  image.declarations = Copy(declarations);
  image.interface_declarations = Copy(program_->interface_declarations);
  image.global_declarations = Copy(program_->global_declarations);
  image.selector_declarations = Copy(program_->selector_declarations);
  image.native_mode = static_cast<std::uint32_t>(program_->native_mode);
  image.global_flags = Copy(program_->global_flags);
  return std::move(data_);
}

void NativeImageData::BindEntries(std::span<const runtime::NativeEntry> entries) {
  for (std::size_t index = 0; index < functions_.size(); ++index) {
    functions_.at(index).entry = entries.subspan(index).front();
  }
}

std::shared_ptr<NativeImageData> DescribeNativeImage(const ir::Program& program,
                                                     std::span<const std::uint8_t> exports) {
  return NativeImageBuilder(program).Build(exports);
}

}  // namespace vela::compiler::native
