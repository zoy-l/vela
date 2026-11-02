#include "compiler/ir/lowerer.h"

#include <bit>
#include <cstddef>
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
      plan.constructor = AddFunctionId("$json.construct." + plan.name);
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
  for (std::size_t index = 0; index < model_.json_decode_plans.size(); ++index) {
    if (program_.json_decode_plans.at(index).kind == JSONDecodeKind::Class) {
      LowerJSONConstructor(model_.json_decode_plans.at(index));
    }
  }
}

void Lowerer::LowerJSONConstructor(const frontend::JSONDecodePlan& plan) {
  using Kind = frontend::TypeDescriptor::Kind;
  constexpr auto kMaskBits = std::numeric_limits<std::uint64_t>::digits;
  const auto& owner = model_.classes.at(plan.type.name);
  const auto& declaration = *owner.declaration;
  const auto location = declaration.location;
  const bool generated = declaration.constructor_kind == frontend::ConstructorKind::Generated;
  const auto count = static_cast<std::uint32_t>(plan.parameters.size());
  const frontend::TypeDescriptor dynamic{.kind = Kind::Dynamic, .name = frontend::kDynamicTypeName};
  const frontend::TypeDescriptor boolean{.kind = Kind::Bool, .name = frontend::kBoolTypeName};
  Function function;
  function.name = "$json.construct." + std::string(plan.type.name);
  function.location = location;
  function.return_type = plan.type;
  function.parameter_count = count * 2;
  function.signature.emplace();
  function.signature->result = RegisterRuntimeType(plan.type);
  for (std::uint32_t parameter = 0; parameter < count * 2; ++parameter) {
    function.signature->parameters.push_back({
        .name = "$argument" + std::to_string(parameter),
        .type = RegisterRuntimeType(parameter < count ? dynamic : boolean),
    });
  }
  BeginFunctionBody(function, &declaration);
  RecordParameterValueTypes(function);
  next_register_ = function.parameter_count;
  std::optional<Construction> construction;
  if (native_mode_ == NativeProgramMode::Reloadable) {
    const auto& identities = declarations_->classes.at(plan.type.name);
    construction.emplace(Construction{
        .type = identities.declaration,
        .supplied = {},
        .generated = generated,
    });
    if (generated) {
      for (const auto& parameter : plan.parameters) {
        construction->supplied.push_back(identities.fields.at(parameter.name));
      }
    }
  }
  const auto object = EmitTemporary({
      .opcode = Opcode::NewInstance,
      .index = class_ids_.at(plan.type.name),
      .location = location,
      .construction = std::move(construction),
  });
  RecordValueType(object, plan.type);
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
    const auto value = LowerConversion({.reg = parameter, .type = dynamic}, source.type, location);
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
