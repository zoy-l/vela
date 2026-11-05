#include "compiler/ir/lowerer.h"

#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
        const auto name = "$construct." + plan.name;
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

void Lowerer::LowerPublicConstructors() {
  for (const auto& source : source_.classes) {
    if (source.export_kind != frontend::ExportKind::Exported ||
        source.constructor_kind == frontend::ConstructorKind::Unavailable ||
        (source.constructor && source.constructor->visibility == frontend::Visibility::Private)) {
      continue;
    }
    const auto name = "$construct." + source.name;
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
  const auto target_name = declaration.name + ".$constructor";
  auto target = function_ids_.find(target_name);
  if (target == function_ids_.end()) {
    static_cast<void>(AddFunctionId(target_name));
    pending_constructors_.push_back(&declaration);
    target = function_ids_.find(target_name);
  }
  const auto base = AllocateRegisterWindow(count, location);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto& parameter = owner.constructor.parameters.at(index);
    const auto value = LowerConversion(
        {.reg = index, .type = current_function_->value_types.at(index)}, parameter.type, location);
    Emit({.opcode = Opcode::Move, .dst = base + index, .lhs = value.reg, .location = location});
  }
  const auto object = EmitTemporary(
      {.opcode = Opcode::Call, .lhs = base, .index = target->second, .location = location});
  RecordValueType(object, owner.type);
  return object;
}

void Lowerer::LowerConstructorFactory(const frontend::ClassDescriptor& owner,
                                      const std::string& name) {
  constexpr auto kMaskBits = std::numeric_limits<std::uint64_t>::digits;
  const auto& declaration = *owner.declaration;
  const auto location = declaration.location;
  const bool generated = declaration.constructor_kind == frontend::ConstructorKind::Generated;
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
    const auto& identities = declarations_->classes.at(owner.type.name);
    construction.emplace(Construction{
        .type = identities.declaration,
        .supplied = {},
        .generated = generated,
    });
    if (generated) {
      for (const auto& parameter : owner.constructor.parameters) {
        construction->supplied.push_back(identities.fields.at(parameter.name));
      }
    }
  }
  const auto object = EmitTemporary({
      .opcode = Opcode::NewInstance,
      .index = class_ids_.at(owner.type.name),
      .location = location,
      .construction = std::move(construction),
  });
  RecordValueType(object, owner.type);
  std::vector<RegisterId> arguments;
  arguments.reserve(ConstructorMaskCount(owner) + count);
  for (std::uint32_t mask = 0; mask < ConstructorMaskCount(owner); ++mask) {
    arguments.push_back(EmitTemporary({.opcode = Opcode::LoadInt, .location = location}));
  }
  for (std::uint32_t parameter = 0; parameter < count; ++parameter) {
    const auto& source = owner.constructor.parameters.at(parameter);
    const auto skip = function.instructions.size();
    if (generated) {
      Emit({.opcode = Opcode::JumpIfFalse, .lhs = count + parameter, .location = location});
    }
    const auto target_type =
        !generated && owner.constructor.optional_parameters.contains(source.name)
            ? frontend::MakeNullableType(source.type)
            : source.type;
    const auto value = LowerConversion(
        {.reg = parameter, .type = function.value_types.at(parameter)}, target_type, location);
    if (generated) {
      const auto field = owner.fields.at(source.name).index;
      Emit({
          .opcode = Opcode::SetField,
          .dst = object,
          .lhs = value.reg,
          .index = field,
          .location = location,
      });
      if (!arguments.empty()) {
        const auto bit = EmitTemporary({
            .opcode = Opcode::LoadInt,
            .immediate = std::bit_cast<std::int64_t>(std::uint64_t{1} << (field % kMaskBits)),
            .location = location,
        });
        const auto mask = arguments.at(field / kMaskBits);
        Emit({
            .opcode = Opcode::BitOrInt,
            .dst = mask,
            .lhs = mask,
            .rhs = bit,
            .location = location,
        });
      }
      BindJump(skip);
    } else {
      arguments.push_back(value.reg);
    }
  }
  if (HasInstanceInitialization(owner) || native_mode_ == NativeProgramMode::Reloadable) {
    EmitConstructorInitialization(owner, object, arguments, location);
  }
  if (native_mode_ == NativeProgramMode::Reloadable) {
    function.instructions.back().completes_construction = true;
  }
  Emit({.opcode = Opcode::Return, .dst = object, .location = location});
  function.register_count = next_register_;
  StoreFunction(std::move(function));
  current_function_ = nullptr;
}

}  // namespace vela::ir
