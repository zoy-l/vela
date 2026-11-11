#include "compiler/native/native_image_data.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "base/source_location.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "runtime/native/native_image.h"
#include "runtime/native_build_identity.h"

namespace vela::compiler::native {
namespace {

static_assert(kInvalidFunctionTarget == runtime::kNoNativeIndex);

runtime::NativeTypeDescription Describe(const layout::Type& type) {
  return {
      .kind = static_cast<std::uint32_t>(type.kind),
      .nullable = static_cast<std::uint32_t>(type.nullable),
      .index = type.index,
  };
}

template <typename Parameter>
std::uint32_t ParameterFlags(const Parameter& parameter) {
  return (parameter.optional ? runtime::kNativeOptionalParameter : 0U) |
         (parameter.rest ? runtime::kNativeRestParameter : 0U);
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

std::uint32_t FunctionFlags(const ir::Function& function) {
  std::uint32_t flags = function.is_async ? runtime::kNativeAsyncFunction : 0;
  if (function.is_generated_constructor) {
    flags |= runtime::kNativeGeneratedConstructor;
  }
  if (function.visibility == frontend::Visibility::Private) {
    flags |= runtime::kNativePrivateFunction;
  }
  if (function.is_exported) {
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

  std::shared_ptr<NativeImageData> Build();

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
    if (const auto found = texts_.find(text); found != texts_.end()) {
      return found->second;
    }
    const auto bytes = Copy(std::span(text));
    const runtime::NativeText result{.data = bytes.data, .size = bytes.size};
    texts_.emplace(result.text(), result);
    return result;
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
  runtime::NativeTable<runtime::NativeCallParameter> CallParameters(
      const CallSignature& source,
      std::span<const std::uint32_t> defaults = {});
  runtime::NativeFunctionDescription Function(const ir::Function& function);
  runtime::NativeAttributeValue AttributeField(const reflection::AttributeValue& field);
  const runtime::NativeFunctionDependencies* Dependencies(const ir::Function& function);
  runtime::NativeTable<runtime::NativeDependencyBlock> Blocks(const ir::Function& function);
  void Reflection();
  [[nodiscard]] DeclarationId LexicalClass(DeclarationId owner) const;

  const ir::Program* program_;
  std::shared_ptr<NativeImageData> data_;
  absl::flat_hash_map<DeclarationId, const ir::Declaration*> declarations_;
  absl::flat_hash_map<std::string_view, runtime::NativeText> texts_;
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
      .native_key = Text(type.native_key),
      .native_contract = Text(type.native_contract),
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
      .constructor = source.constructor,
      .enum_members = Texts(source.enum_members),
  };
}

runtime::NativeSignatureDescription NativeImageBuilder::Signature(const layout::Signature& source) {
  std::vector<runtime::NativeSignatureParameter> parameters;
  parameters.reserve(source.parameters.size());
  for (const auto& parameter : source.parameters) {
    parameters.push_back({
        .type = Describe(parameter.type),
        .flags = ParameterFlags(parameter),
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
  if (function.json_decode_plan) {
    for (const auto node : ir::JSONDecodeClosure(*program_, *function.json_decode_plan)) {
      const auto target = program_->json_decode_plans.at(node).constructor;
      if (target != kInvalidFunctionTarget) {
        versions.push_back(target);
        version_positions.push_back(static_cast<std::uint32_t>(function.instructions.size() - 2));
      }
    }
  }
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

runtime::NativeTable<runtime::NativeCallParameter> NativeImageBuilder::CallParameters(
    const CallSignature& source,
    std::span<const std::uint32_t> defaults) {
  std::vector<runtime::NativeCallParameter> parameters;
  parameters.reserve(source.parameters.size());
  for (const auto& parameter : source.parameters) {
    parameters.push_back({
        .name = Text(parameter.name),
        .type = parameter.type,
        .flags = ParameterFlags(parameter),
    });
  }
  for (const auto index : defaults) {
    parameters[index].flags |= runtime::kNativeDefaultParameter;
  }
  return Copy(parameters);
}

runtime::NativeFunctionDescription NativeImageBuilder::Function(const ir::Function& function) {
  runtime::NativeFunctionDescription result{
      .name = Text(function.name),
      .native_symbol = Text(function.native_symbol),
      .parameters = {},
      .locations = {},
      .parameter_count = function.parameter_count,
      .flags = FunctionFlags(function),
      .identity = function.identity,
      .dependencies = Dependencies(function),
  };
  if (function.signature) {
    const auto& signature = *function.signature;
    result.flags |= runtime::kNativeTypedFunction;
    result.hidden_parameters = signature.hidden_parameters;
    result.receiver_type = signature.receiver.value_or(runtime::kNoNativeIndex);
    result.result_type = signature.result.value_or(runtime::kNoNativeIndex);
    result.parameters = CallParameters(signature);
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

std::shared_ptr<NativeImageData> NativeImageBuilder::Build() {
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
  for (const auto& function : program_->functions) {
    functions.push_back(Function(function));
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
  std::vector<runtime::NativeJSONDecodePlan> json_plans;
  for (const auto& plan : program_->json_decode_plans) {
    std::vector<runtime::NativeJSONDecodeParameter> parameters;
    parameters.reserve(plan.parameters.size());
    for (const auto& parameter : plan.parameters) {
      parameters.push_back({
          .name = Text(parameter.name),
          .node = parameter.node,
          .optional = static_cast<std::uint32_t>(parameter.optional),
      });
    }
    json_plans.push_back({
        .name = Text(plan.name),
        .parameters = Copy(parameters),
        .kind = plan.kind,
        .type = plan.type,
        .element = plan.element,
        .constructor = plan.constructor,
    });
  }
  image.json_decode_plans = Copy(json_plans);
  Reflection();
  return std::move(data_);
}

runtime::NativeAttributeValue NativeImageBuilder::AttributeField(
    const reflection::AttributeValue& field) {
  runtime::NativeAttributeValue value{.type = field.type, .payload = 0, .text = {}};
  std::visit(
      [&](const auto& scalar) {
        using T = std::decay_t<decltype(scalar)>;
        if constexpr (std::is_same_v<T, std::string>) {
          value.kind = static_cast<std::uint32_t>(runtime::NativeAttributeValueKind::String);
          value.text = Text(scalar);
        } else if constexpr (std::is_same_v<T, double>) {
          value.kind = static_cast<std::uint32_t>(runtime::NativeAttributeValueKind::Double);
          value.payload = std::bit_cast<std::uint64_t>(scalar);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          value.kind = static_cast<std::uint32_t>(runtime::NativeAttributeValueKind::Integer);
          value.payload = std::bit_cast<std::uint64_t>(scalar);
        } else if constexpr (std::is_same_v<T, bool>) {
          value.kind = static_cast<std::uint32_t>(runtime::NativeAttributeValueKind::Boolean);
          value.payload = static_cast<std::uint64_t>(scalar);
        }
      },
      field.value);
  return value;
}

void NativeImageBuilder::Reflection() {
  const auto& layouts = program_->layouts;
  const auto argument_keys = [&](const auto& arguments) {
    std::vector<std::string> keys;
    keys.reserve(arguments.size());
    for (const auto argument : arguments) {
      keys.push_back(argument == runtime::kNoNativeIndex
                         ? "void"
                         : layout::TypeKey(layouts, layouts.types.at(argument)));
    }
    return keys;
  };
  using TypeOrder = std::tuple<std::string, std::string, std::vector<std::string>, std::size_t>;
  std::vector<TypeOrder> types;
  for (std::size_t index = 0; index < program_->reflection_types.size(); ++index) {
    const auto& type = program_->reflection_types.at(index);
    types.emplace_back(type.module, type.name, argument_keys(type.arguments), index);
  }
  std::ranges::sort(types);
  absl::flat_hash_map<std::uint32_t, std::size_t> type_order;
  std::vector<runtime::NativeReflectionType> reflection_types;
  for (const auto& order : types) {
    const auto& type = program_->reflection_types.at(std::get<3>(order));
    type_order.emplace(type.type, reflection_types.size());
    reflection_types.push_back({
        .type = type.type,
        .module = Text(type.module),
        .name = Text(type.name),
        .arguments = Copy(type.arguments),
        .base = type.base,
        .interfaces = Copy(type.interfaces),
        .flags = type.flags,
    });
  }
  data_->image_.reflection_types = Copy(reflection_types);
  using MemberOrder = std::tuple<std::string, std::vector<std::string>, std::size_t>;
  std::vector<MemberOrder> members;
  for (std::size_t index = 0; index < program_->reflection_members.size(); ++index) {
    const auto& member = program_->reflection_members.at(index);
    members.emplace_back(member.name, argument_keys(member.type_arguments), index);
  }
  std::ranges::sort(members);
  std::vector<runtime::NativeReflectionMember> reflection_members;
  for (const auto& order : members) {
    const auto& member = program_->reflection_members.at(std::get<2>(order));
    reflection_members.push_back({
        .declaration = member.declaration,
        .declaring_type = member.declaring_type,
        .name = Text(member.name),
        .kind = static_cast<std::uint32_t>(member.kind),
        .flags = member.flags,
        .type = member.type,
        .target = member.target,
        .getter = member.getter,
        .setter = member.setter,
        .parameters = CallParameters(member.signature, member.default_parameters),
        .type_arguments = Copy(member.type_arguments),
    });
  }
  data_->image_.reflection_members = Copy(reflection_members);
  absl::flat_hash_map<DeclarationId, const MemberOrder*> attribute_targets;
  for (const auto& order : members) {
    const auto& member = program_->reflection_members.at(std::get<2>(order));
    attribute_targets.emplace(member.declaration, &order);
  }
  using AttributeOrder =
      std::tuple<std::size_t, std::string, std::vector<std::string>, std::size_t>;
  std::vector<AttributeOrder> annotation_order;
  for (std::size_t index = 0; index < program_->attributes.size(); ++index) {
    const auto& attribute = program_->attributes.at(index);
    if (const auto found = attribute_targets.find(attribute.target);
        found != attribute_targets.end()) {
      const auto& order = *found->second;
      const auto& member = program_->reflection_members.at(std::get<2>(order));
      annotation_order.emplace_back(type_order.at(member.declaring_type), member.name,
                                    std::get<1>(order), index);
    } else {
      annotation_order.emplace_back(types.size(), "", std::vector<std::string>{}, index);
    }
  }
  std::ranges::sort(annotation_order);
  std::vector<runtime::NativeAttribute> attributes;
  for (const auto& order : annotation_order) {
    const auto& attribute = program_->attributes.at(std::get<3>(order));
    std::vector<runtime::NativeAttributeValue> fields;
    fields.reserve(attribute.fields.size());
    for (const auto& field : attribute.fields) {
      fields.push_back(AttributeField(field));
    }
    attributes.push_back(
        {.target = attribute.target, .type = attribute.type, .fields = Copy(fields)});
  }
  data_->image_.attributes = Copy(attributes);
}

void NativeImageData::BindEntries(std::span<const runtime::NativeEntry> entries,
                                  std::span<const runtime::NativeTypedEntry> typed_entries) {
  for (std::size_t index = 0; index < functions_.size(); ++index) {
    auto& function = functions_.at(index);
    function.entry = entries[index];
    function.typed_entry = typed_entries[index];
  }
}

std::shared_ptr<NativeImageData> DescribeNativeImage(const ir::Program& program) {
  return NativeImageBuilder(program).Build();
}

}  // namespace vela::compiler::native
