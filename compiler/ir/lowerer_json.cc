#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "base/callable.h"
#include "base/json_decode.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

JSONDecodeKind DecodeKind(const frontend::TypeDescriptor& type) {
  using Kind = frontend::TypeDescriptor::Kind;
  if (type.nullable) {
    return JSONDecodeKind::Nullable;
  }
  switch (type.kind) {
    case Kind::Bool:
      return JSONDecodeKind::Bool;
    case Kind::Int:
      return JSONDecodeKind::Int;
    case Kind::Double:
      return JSONDecodeKind::Double;
    case Kind::String:
      return JSONDecodeKind::String;
    case Kind::Array:
      return JSONDecodeKind::Array;
    case Kind::Map:
      return JSONDecodeKind::Map;
    case Kind::Class:
      return JSONDecodeKind::Class;
    default:
      return JSONDecodeKind::Dynamic;
  }
}

}  // namespace

void Lowerer::LowerJSONDecodePlans() {
  for (const auto& source : model_.json_decode_plans) {
    JSONDecodePlan plan{
        .name = std::string(source.type.name),
        .kind = DecodeKind(source.type),
        .type = RegisterRuntimeType(source.type),
        .element = source.element.value_or(0),
        .constructor = kInvalidFunctionTarget,
        .parameters = {},
    };
    if (plan.kind == JSONDecodeKind::Class) {
      const auto class_index = class_ids_.at(plan.name);
      auto constructor = program_.classes.at(class_index).constructor;
      if (constructor == kInvalidFunctionTarget) {
        const auto name = InitializerKey(InitializerKind::ConstructorFactory, plan.name);
        constructor = AddFunctionId(name);
        program_.classes.at(class_index).constructor = constructor;
        LowerConstructorFactory(model_.classes.at(plan.name), name);
      }
      plan.constructor = constructor;
    }
    for (const auto& parameter : source.parameters) {
      plan.parameters.push_back({
          .name = parameter.name,
          .node = parameter.node,
          .optional = parameter.optional,
      });
    }
    program_.json_decode_plans.push_back(std::move(plan));
  }
}

void Lowerer::LowerExposedConstructors() {
  absl::flat_hash_set<std::string_view> registered;
  for (const auto& type : model_.reflection) {
    if (std::ranges::any_of(type.members, [](const auto& member) {
          return member.kind == frontend::ReflectionMemberKind::Constructor;
        })) {
      registered.insert(type.type.name);
    }
  }
  for (const auto& source : source_.classes) {
    if ((source.export_kind != frontend::ExportKind::Exported &&
         !registered.contains(source.name)) ||
        source.constructor_kind == frontend::ConstructorKind::Unavailable ||
        (source.constructor && source.constructor->visibility == frontend::Visibility::Private)) {
      continue;
    }
    const auto name = InitializerKey(InitializerKind::ConstructorFactory, source.name);
    const auto target = AddFunctionId(name);
    program_.classes.at(class_ids_.at(source.name)).constructor = target;
    LowerConstructorFactory(model_.classes.at(source.name), name);
  }
}

Function Lowerer::PrepareConstructorFactory(const frontend::ClassDescriptor& owner,
                                            const std::string& name) {
  using Kind = frontend::TypeDescriptor::Kind;
  const auto& declaration = *owner.declaration;
  const auto location = declaration.location;
  const auto count = static_cast<std::uint32_t>(owner.constructor.parameters.size());
  const frontend::TypeDescriptor boolean{.kind = Kind::Bool, .name = frontend::kBoolTypeName};
  Function function;
  function.name = name;
  function.location = location;
  function.return_type = owner.type;
  function.parameter_count = count * 2;
  function.signature.emplace();
  function.signature->result = RegisterRuntimeType(owner.type);
  for (std::uint32_t parameter = 0; parameter < count * 2; ++parameter) {
    function.signature->parameters.push_back({
        .name = parameter < count ? owner.constructor.parameters.at(parameter).name
                                  : "$supplied." + std::to_string(parameter - count),
        .type = RegisterRuntimeType(
            parameter < count ? owner.constructor.parameters.at(parameter).type : boolean),
        .optional = parameter < count && owner.constructor.optional_parameters.contains(
                                             owner.constructor.parameters.at(parameter).name),
    });
  }
  return function;
}

RegisterId Lowerer::LowerHostConstructorFactory(const frontend::ClassDescriptor& owner) {
  const auto& declaration = *owner.declaration;
  const auto location = declaration.location;
  const auto count = static_cast<std::uint32_t>(owner.constructor.parameters.size());
  const auto target = ConstructorTarget(owner);
  const auto base = AllocateRegisterWindow(count, location);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto& parameter = owner.constructor.parameters.at(index);
    const auto value = LowerConversion(
        {.reg = index, .type = current_function_->value_types.at(index)}, parameter.type, location);
    Emit({.opcode = Opcode::Move, .dst = base + index, .lhs = value.reg, .location = location});
  }
  const auto object =
      EmitTemporary({.opcode = Opcode::Call, .lhs = base, .index = target, .location = location});
  RecordValueType(object, owner.type);
  return object;
}

void Lowerer::LowerConstructorFactory(const frontend::ClassDescriptor& owner,
                                      const std::string& name) {
  const auto& declaration = *owner.declaration;
  const auto location = declaration.location;
  const auto count = static_cast<std::uint32_t>(owner.constructor.parameters.size());
  auto function = PrepareConstructorFactory(owner, name);
  BeginFunctionBody(function, &declaration);
  RecordParameterValueTypes(function);
  next_register_ = function.parameter_count;
  if (!declaration.native_key.empty()) {
    const auto object = LowerHostConstructorFactory(owner);
    Emit({.opcode = Opcode::Return, .dst = object, .location = location});
    function.register_count = next_register_;
    StoreFunction(std::move(function));
    current_function_ = nullptr;
    return;
  }
  std::optional<Construction> construction;
  if (native_mode_ == NativeProgramMode::Reloadable) {
    std::vector<std::uint32_t> required;
    if (owner.UsesFieldConstructor()) {
      required.reserve(count);
      for (std::uint32_t index = 0; index < count; ++index) {
        if (!owner.constructor.parameters[index].has_default) {
          required.push_back(index);
        }
      }
    }
    construction = DescribeConstruction(owner, required);
  }
  const auto object = EmitTemporary({
      .opcode = Opcode::NewInstance,
      .index = class_ids_.at(owner.type.name),
      .location = location,
      .construction = std::move(construction),
  });
  RecordValueType(object, owner.type);
  const bool needs_presence = owner.UsesFieldConstructor() || owner.constructor.HasDefaults();
  std::vector<ConstructorArgument> parameters;
  parameters.reserve(count);
  for (std::uint32_t parameter = 0; parameter < count; ++parameter) {
    parameters.push_back({
        .value = parameter,
        .supplied = needs_presence ? std::optional(count + parameter) : std::nullopt,
    });
  }
  LowerBoundConstructorCall(owner, object, parameters, location);
  if (native_mode_ == NativeProgramMode::Reloadable) {
    function.instructions.back().completes_construction = true;
  }
  Emit({.opcode = Opcode::Return, .dst = object, .location = location});
  function.register_count = next_register_;
  StoreFunction(std::move(function));
  current_function_ = nullptr;
}

}  // namespace vela::ir
