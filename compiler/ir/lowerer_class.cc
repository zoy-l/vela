#include "base/callable.h"
#include "base/declaration.h"
#include "base/layout_description.h"
#include "base/native_binding.h"
#include "base/native_program_mode.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/layout_description.h"

namespace vela::ir {
void Lowerer::BuildNativeClassContracts() {
  struct Contract final {
    ClassId owner;
    std::vector<Function> members;
  };
  std::vector<Contract> contracts;
  for (const auto& owner : source_.classes) {
    if (owner.native_key.empty()) {
      continue;
    }
    const auto& descriptor = model_.classes.at(owner.name);
    Contract contract{.owner = class_ids_.at(owner.name), .members = {}};
    if (owner.constructor != nullptr) {
      contract.members.push_back(
          PrepareFunction(*owner.constructor, descriptor.constructor, &owner, nullptr));
    }
    for (const auto& method : owner.methods) {
      contract.members.push_back(
          PrepareFunction(method, descriptor.methods.at(method.name), &owner, nullptr));
    }
    contracts.push_back(std::move(contract));
  }
  // 全部公开成员（包括未调用的构造器）先登记类型，再从生产布局生成同一份规范摘要。
  program_.layouts = DescribeLayouts(program_, model_, source_);
  const auto& layouts = program_.layouts;
  for (const auto& contract : contracts) {
    std::vector<NativeContractMember> members;
    for (const auto& function : contract.members) {
      if (!function.signature) {
        std::unreachable();
      }
      const auto& signature = *function.signature;
      NativeContractMember member{
          .key = function.native_symbol,
          .parameters = {},
          .result = signature.result ? layout::TypeKey(layouts, layouts.types.at(*signature.result))
                                     : "void",
      };
      for (const auto& parameter : signature.parameters) {
        member.parameters.push_back({
            .name = parameter.name,
            .type = layout::TypeKey(layouts, layouts.types.at(parameter.type)),
        });
      }
      members.push_back(std::move(member));
    }
    auto key = NativeClassContract(std::move(members)).Hex();
    program_.classes.at(contract.owner).native_contract = key;
    program_.layouts.classes.at(contract.owner).native_contract = std::move(key);
  }
}

void Lowerer::LowerFieldDefaultInitializers(const frontend::Class& owner) {
  const auto& info = model_.classes.at(owner.name);
  for (const auto& field : owner.fields) {
    if (field.storage == frontend::MemberStorage::Static) {
      LowerInitializer(
          InitializerKey(InitializerKind::StaticField,
                         QualifiedMemberKey({.owner = owner.name, .name = field.name})),
          *field.initializer, info.static_fields.at(field.name).type, field.location);
    } else if (native_mode_ == NativeProgramMode::Reloadable && field.initializer != nullptr) {
      LowerInitializer(
          InitializerKey(InitializerKind::InstanceField,
                         QualifiedMemberKey({.owner = owner.name, .name = field.name})),
          *field.initializer, info.fields.at(info.FieldKey(field.name)).type, field.location);
    }
  }
}

bool Lowerer::HasInstanceInitialization(const frontend::ClassDescriptor& owner) {
  if (const auto found = instance_initialization_.find(owner.type.name);
      found != instance_initialization_.end()) {
    return found->second;
  }
  const auto& source = *owner.declaration;
  const auto initialized =
      source.constructor ||
      (!owner.UsesFieldConstructor() && !owner.constructor.parameters.empty()) ||
      std::ranges::any_of(source.fields,
                          [](const auto& field) {
                            return field.storage == frontend::MemberStorage::Instance &&
                                   field.initializer;
                          }) ||
      (owner.base_class && HasInstanceInitialization(model_.classes.at(*owner.base_class)));
  instance_initialization_.emplace(owner.type.name, initialized);
  return initialized;
}

bool Lowerer::NeedsConstructorInitialization(const frontend::ClassDescriptor& owner) {
  return (native_mode_ == NativeProgramMode::Reloadable && owner.UsesFieldConstructor()) ||
         HasInstanceInitialization(owner);
}

std::uint32_t Lowerer::ConstructorMaskCount(const frontend::ClassDescriptor& owner) const {
  if (native_mode_ == NativeProgramMode::Reloadable) {
    return 0;
  }
  const bool has_optional_fields =
      owner.UsesFieldConstructor() && !owner.constructor.optional_parameters.empty();
  return has_optional_fields
             ? static_cast<std::uint32_t>((owner.field_order.size() + kConstructorMaskBits - 1) /
                                          kConstructorMaskBits)
             : 0;
}

Lowerer::ConstructorFieldMask Lowerer::ConstructorMask(FieldId field) {
  return {
      .index = field / kConstructorMaskBits,
      .bit = std::uint64_t{1} << (field % kConstructorMaskBits),
  };
}

void Lowerer::LowerConstructorInitializers(const frontend::ClassDescriptor& owner) {
  const auto& source = *owner.declaration;
  const auto field_arguments = owner.UsesFieldConstructor();
  const auto count = static_cast<RegisterId>(owner.constructor.parameters.size());
  std::vector<std::optional<std::uint32_t>> field_parameters;
  if (!field_arguments) {
    field_parameters.resize(owner.field_order.size());
    for (std::uint32_t parameter = 0; parameter < count; ++parameter) {
      if (const auto* const field = owner.constructor_bindings.at(parameter).field) {
        field_parameters[field->index] = parameter;
      }
    }
  }
  for (const auto& field : source.fields) {
    if (field.storage == frontend::MemberStorage::Static) {
      continue;
    }
    const auto& descriptor = *owner.FindField(field.name);
    const auto index = descriptor.index;
    const auto bound_parameter = field_arguments ? std::nullopt : field_parameters[index];
    if (bound_parameter) {
      LowerConstructorField(owner, field, descriptor, *bound_parameter);
      continue;
    }
    if (!field.initializer) {
      continue;
    }
    if (native_mode_ == NativeProgramMode::Reloadable && field_arguments) {
      // 显式实参已按稳定字段身份写入对象；读取只为尚未提供的槽运行当前初始化器。
      const auto value = EmitTemporary({
          .opcode = Opcode::GetField,
          .lhs = 0,
          .index = index,
          .location = field.location,
          .field_declaration = FieldDeclaration(owner.type, index),
      });
      RecordValueType(value, descriptor.type);
      continue;
    }
    std::optional<std::size_t> provided;
    if (source.IsConstructorField(field)) {
      const auto field_mask = ConstructorMask(index);
      const auto mask = EmitTemporary({
          .opcode = Opcode::LoadInt,
          .immediate = std::bit_cast<std::int64_t>(field_mask.bit),
          .location = field.location,
      });
      const auto bits = EmitTemporary({
          .opcode = Opcode::BitAndInt,
          .lhs = 1 + field_mask.index,
          .rhs = mask,
          .location = field.location,
      });
      const auto zero = EmitTemporary({.opcode = Opcode::LoadInt, .location = field.location});
      const auto missing = EmitTemporary(
          {.opcode = Opcode::EqualInt, .lhs = bits, .rhs = zero, .location = field.location});
      provided = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfFalse, .lhs = missing, .location = field.location});
    }
    const auto value = LowerExpression(*field.initializer);
    Emit({
        .opcode = Opcode::SetField,
        .dst = 0,
        .lhs = value.reg,
        .index = index,
        .location = field.location,
        .field_declaration = FieldDeclaration(owner.type, index),
    });
    if (provided) {
      BindJump(*provided);
    }
  }
  LowerBaseConstructor(owner);
}

void Lowerer::LowerConstructorField(const frontend::ClassDescriptor& owner,
                                    const frontend::Field& field,
                                    const frontend::ClassDescriptor::Field& descriptor,
                                    std::uint32_t parameter) {
  const auto index = descriptor.index;
  const auto& parameters = owner.constructor.parameters;
  const auto count = static_cast<RegisterId>(parameters.size());
  const auto& binding = parameters[parameter];
  const auto input = 1 + parameter;
  const LoweredValue argument{.reg = input, .type = current_function_->value_types.at(input)};
  const auto value = binding.has_default
                         ? LowerDefaultValue(*field.initializer, argument, 1 + count + parameter,
                                             descriptor.type, field.location)
                         : LowerConversion(argument, descriptor.type, field.location).reg;
  Emit({
      .opcode = Opcode::SetField,
      .dst = 0,
      .lhs = value,
      .index = index,
      .location = field.location,
      .field_declaration = FieldDeclaration(owner.type, index),
  });
}

void Lowerer::LowerBaseConstructor(const frontend::ClassDescriptor& owner) {
  const auto& source = *owner.declaration;
  if (!source.base_class || source.HasExplicitSuperCall()) {
    return;
  }
  const auto& base = model_.classes.at(source.base_class->name);
  if (owner.UsesFieldConstructor() && base.UsesFieldConstructor()) {
    if (!NeedsConstructorInitialization(base)) {
      return;
    }
    const auto count = ConstructorMaskCount(base);
    std::vector<RegisterId> arguments;
    arguments.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
      arguments.push_back(index + 1);
    }
    // 字段实参已写入接收者；这里只转发字段掩码，默认值仍由父类求值。
    EmitConstructorInitialization(base, 0, arguments, source.location);
    return;
  }
  const auto count = static_cast<RegisterId>(owner.constructor.parameters.size());
  std::vector<ConstructorArgument> parameters(base.constructor.parameters.size());
  const bool needs_presence = base.UsesFieldConstructor() || base.constructor.HasDefaults();
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto target = owner.constructor_bindings.at(index).base_parameter;
    if (target) {
      auto& parameter = parameters[*target];
      parameter.value = 1 + index;
      if (needs_presence && owner.constructor.parameters[index].has_default) {
        parameter.supplied = 1 + count + index;
      }
    }
  }
  LowerBoundConstructorCall(base, 0, parameters, source.location);
}

ClassId Lowerer::RegisterEnumType(const frontend::TypeDescriptor& type) {
  const auto name = frontend::NonNullableTypeName(type);
  if (const auto existing = class_ids_.find(name); existing != class_ids_.end()) {
    return existing->second;
  }
  const auto index = static_cast<ClassId>(program_.classes.size());
  Class layout;
  layout.name = name;
  layout.kind = Class::Kind::Enum;
  const auto declaration = std::ranges::find(source_.enums, name, &frontend::Enum::name);
  for (const auto& member : declaration->members) {
    layout.enum_members.push_back(member.name);
  }
  layout.declaration = declarations_->nominals.at(name);
  layout.field_types = {
      {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName},
  };
  program_.classes.push_back(std::move(layout));
  class_ids_.emplace(name, index);
  return index;
}

std::uint32_t Lowerer::RegisterRuntimeType(const frontend::TypeDescriptor& type) {
  auto found = runtime_type_ids_.find(type.name);
  std::uint32_t index = 0;
  if (found == runtime_type_ids_.end()) {
    index = static_cast<std::uint32_t>(program_.runtime_types.size());
    program_.runtime_types.push_back(type);
    runtime_type_ids_.emplace(type.name, index);
    if (type.kind == frontend::TypeDescriptor::Kind::Enum) {
      static_cast<void>(RegisterEnumType(type));
    }
    if (type.kind == frontend::TypeDescriptor::Kind::Array) {
      static_cast<void>(RegisterArrayType(type));
    }
    if (type.kind == frontend::TypeDescriptor::Kind::Set ||
        type.kind == frontend::TypeDescriptor::Kind::Map) {
      static_cast<void>(RegisterCollectionType(type));
    }
  } else {
    index = found->second;
  }
  return index;
}

Lowerer::LoweredValue Lowerer::LowerTypeOperation(const frontend::Expression& expression) {
  const auto& target = model_.type_operands.at(&expression);
  auto value = LowerExpression(*expression.operand);
  if (expression.kind == frontend::Expression::Kind::TypeCast) {
    return LowerConversion(value, target, expression.location);
  }
  if (value.type.kind == frontend::TypeDescriptor::Kind::Enum) {
    value = LowerConversion(
        value,
        {.kind = frontend::TypeDescriptor::Kind::Dynamic, .name = frontend::kDynamicTypeName},
        expression.location);
  }
  const auto index = RegisterRuntimeType(target);
  const auto result = AllocateRegister(expression.location);
  Emit({
      .opcode = expression.kind == frontend::Expression::Kind::TypeTest ? Opcode::IsType
                                                                        : Opcode::CheckedCast,
      .dst = result,
      .lhs = value.reg,
      .index = index,
      .location = expression.location,
  });
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

void Lowerer::LowerInitializer(std::string name,
                               const frontend::Expression& initializer,
                               const frontend::TypeDescriptor& type,
                               frontend::SourceLocation location) {
  Function function;
  function.name = std::move(name);
  function.return_type = type;
  function.signature.emplace();
  function.signature->result = RegisterRuntimeType(type);
  BeginFunctionBody(function);
  const auto value = LowerExpression(initializer);
  Emit({.opcode = Opcode::Return, .dst = value.reg, .location = location});
  function.register_count = next_register_;
  StoreFunction(std::move(function));
  current_function_ = nullptr;
}

Lowerer::LoweredValue Lowerer::LowerConstruction(const frontend::Expression& expression,
                                                 const frontend::TypeDescriptor& expected) {
  const auto& info = model_.classes.at(expected.name);
  if (!info.declaration->native_key.empty()) {
    return LowerInvocation(expression, info.constructor, ConstructorTarget(info), nullptr);
  }
  std::vector<LoweredValue> evaluated;
  std::optional<Construction> construction;
  if (native_mode_ == NativeProgramMode::Reloadable) {
    // 实参可能 await；在分配对象及进入初始化屏障前求值并快照，避免挂起半初始化实例。
    for (const auto& argument : expression.arguments) {
      const auto value = LowerExpression(*argument.value);
      const auto saved = AllocateRegister(argument.location);
      Emit({.opcode = Opcode::Move, .dst = saved, .lhs = value.reg, .location = argument.location});
      evaluated.push_back({.reg = saved, .type = value.type});
    }
    construction = DescribeConstruction(info, model_.argument_order.at(&expression));
  }
  const auto object = AllocateRegister(expression.location);
  Emit({
      .opcode = Opcode::NewInstance,
      .dst = object,
      .index = class_ids_.at(expected.name),
      .location = expression.location,
      .construction = std::move(construction),
  });
  const auto result =
      LowerConstructorCall(expression.arguments, model_.argument_order.at(&expression),
                           expression.location, info, object, evaluated);
  if (native_mode_ == NativeProgramMode::Reloadable) {
    current_function_->instructions.back().completes_construction = true;
  }
  return result;
}

std::vector<RegisterId> Lowerer::PrepareConstructorArguments(
    const frontend::FunctionDescriptor& signature,
    std::span<const std::uint32_t> supplied,
    frontend::SourceLocation location) {
  const auto count = signature.parameters.size();
  std::vector<RegisterId> arguments(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (signature.parameters[index].has_default) {
      arguments[index] = EmitTemporary({.opcode = Opcode::LoadNull, .location = location});
    }
  }
  if (signature.HasDefaults()) {
    const auto presence = AllocateRegisterWindow(count, location);
    InitializeArgumentPresence(signature, supplied, presence, location);
    for (std::size_t index = 0; index < count; ++index) {
      arguments.push_back(presence + static_cast<RegisterId>(index));
    }
  }
  return arguments;
}

Construction Lowerer::DescribeConstruction(const frontend::ClassDescriptor& owner,
                                           std::span<const std::uint32_t> supplied) const {
  Construction construction{
      .type = declarations_->classes.at(owner.type.name).declaration,
      .supplied = {},
      .generated = owner.UsesFieldConstructor(),
  };
  if (!construction.generated) {
    return construction;
  }
  const auto& fields = program_.classes.at(class_ids_.at(owner.type.name)).fields;
  std::vector<bool> parameters(fields.size());
  for (const auto& binding : owner.constructor_bindings) {
    parameters[binding.field->index] = true;
  }
  for (const auto index : supplied) {
    construction.supplied.push_back(
        fields[owner.constructor_bindings[index].field->index].declaration);
  }
  // 构造体确定性写入的非参数字段也属于旧调用的初始化证明；新增必填字段仍要求 Reload。
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto& field = fields[index];
    if (!parameters[index] && field.initializer == kInvalidFunctionTarget) {
      construction.supplied.push_back(field.declaration);
    }
  }
  return construction;
}

Lowerer::LoweredValue Lowerer::LowerConstructorCall(
    std::span<const frontend::CallArgument> call_arguments,
    std::span<const std::uint32_t> argument_order,
    frontend::SourceLocation location,
    const frontend::ClassDescriptor& info,
    RegisterId object,
    std::span<const LoweredValue> evaluated) {
  const auto& signature = info.constructor;
  const bool generated = info.UsesFieldConstructor();
  const auto masks = ConstructorMaskCount(info);
  auto arguments = generated ? std::vector<RegisterId>{}
                             : PrepareConstructorArguments(signature, argument_order, location);
  std::vector<std::uint64_t> provided(masks, 0);
  // 自动构造的实参依源码顺序直接写入尚未逸出的对象，避免为全部字段保留寄存器。
  for (std::size_t index = 0; index < call_arguments.size(); ++index) {
    const auto& argument = call_arguments[index];
    const auto parameter = argument_order[index];
    const auto value = evaluated.empty() ? LowerExpression(*argument.value) : evaluated[index];
    if (generated) {
      const auto field = info.constructor_bindings[parameter].field->index;
      Emit({
          .opcode = Opcode::SetField,
          .dst = object,
          .lhs = value.reg,
          .index = field,
          .location = argument.location,
          .field_declaration = FieldDeclaration(info.type, field),
      });
      if (masks != 0) {
        const auto field_mask = ConstructorMask(field);
        provided.at(field_mask.index) |= field_mask.bit;
      }
    } else {
      const auto saved = AllocateRegister(argument.location);
      Emit({.opcode = Opcode::Move, .dst = saved, .lhs = value.reg, .location = argument.location});
      arguments.at(parameter) = saved;
    }
  }
  if (NeedsConstructorInitialization(info)) {
    if (generated) {
      for (const auto mask : provided) {
        arguments.push_back(EmitTemporary({
            .opcode = Opcode::LoadInt,
            .immediate = std::bit_cast<std::int64_t>(mask),
            .location = location,
        }));
      }
    }
    EmitConstructorInitialization(info, object, arguments, location);
  }
  return {.reg = object, .type = info.type};
}

std::uint32_t Lowerer::ConstructorTarget(const frontend::ClassDescriptor& owner) {
  const auto name = MethodKey(owner.type.name, DeclarationKind::Method, frontend::kConstructorName);
  if (const auto existing = function_ids_.find(name); existing != function_ids_.end()) {
    return existing->second;
  }
  const auto target = AddFunctionId(name);
  pending_constructors_.push_back(owner.declaration);
  return target;
}

std::vector<RegisterId> Lowerer::LowerFieldConstructorArguments(
    const frontend::ClassDescriptor& owner,
    RegisterId object,
    std::span<const ConstructorArgument> parameters,
    frontend::SourceLocation location) {
  std::vector<RegisterId> arguments;
  const auto masks = ConstructorMaskCount(owner);
  std::vector<std::uint64_t> provided(masks);
  if (masks != 0) {
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      const auto& parameter = parameters[index];
      if (parameter.value && !parameter.supplied) {
        const auto field_mask = ConstructorMask(owner.constructor_bindings[index].field->index);
        provided[field_mask.index] |= field_mask.bit;
      }
    }
  }
  arguments.reserve(masks);
  for (std::uint32_t mask = 0; mask < masks; ++mask) {
    arguments.push_back(EmitTemporary({
        .opcode = Opcode::LoadInt,
        .immediate = std::bit_cast<std::int64_t>(provided[mask]),
        .location = location,
    }));
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto& parameter = parameters[index];
    if (!parameter.value) {
      continue;
    }
    std::optional<std::size_t> skip;
    if (parameter.supplied) {
      skip = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfFalse, .lhs = *parameter.supplied, .location = location});
    }
    const auto field = owner.constructor_bindings[index].field->index;
    const auto input = *parameter.value;
    const auto value =
        LowerConversion({.reg = input, .type = current_function_->value_types.at(input)},
                        owner.constructor.parameters[index].type, location);
    Emit({
        .opcode = Opcode::SetField,
        .dst = object,
        .lhs = value.reg,
        .index = field,
        .location = location,
        .field_declaration = FieldDeclaration(owner.type, field),
    });
    if (masks != 0 && parameter.supplied) {
      const auto field_mask = ConstructorMask(field);
      const auto bit = EmitTemporary({
          .opcode = Opcode::LoadInt,
          .immediate = std::bit_cast<std::int64_t>(field_mask.bit),
          .location = location,
      });
      const auto mask = arguments.at(field_mask.index);
      Emit({
          .opcode = Opcode::BitOrInt,
          .dst = mask,
          .lhs = mask,
          .rhs = bit,
          .location = location,
      });
    }
    if (skip) {
      BindJump(*skip);
    }
  }
  return arguments;
}

void Lowerer::LowerBoundConstructorCall(const frontend::ClassDescriptor& owner,
                                        RegisterId object,
                                        std::span<const ConstructorArgument> parameters,
                                        frontend::SourceLocation location) {
  std::vector<RegisterId> arguments;
  if (owner.UsesFieldConstructor()) {
    arguments = LowerFieldConstructorArguments(owner, object, parameters, location);
  } else {
    const bool needs_presence = owner.constructor.HasDefaults();
    arguments.reserve(parameters.size() * (needs_presence ? 2 : 1));
    std::optional<RegisterId> empty;
    std::array<std::optional<RegisterId>, 2> presence;
    const auto constant = [&](std::optional<RegisterId>& cached, Opcode opcode,
                              std::int64_t immediate = 0) {
      if (!cached) {
        cached = EmitTemporary({.opcode = opcode, .immediate = immediate, .location = location});
      }
      return *cached;
    };
    for (const auto& parameter : parameters) {
      arguments.push_back(parameter.value ? *parameter.value : constant(empty, Opcode::LoadNull));
    }
    if (needs_presence) {
      for (const auto& parameter : parameters) {
        const auto present = parameter.value.has_value();
        arguments.push_back(parameter.supplied
                                ? *parameter.supplied
                                : constant(presence[static_cast<std::size_t>(present)],
                                           Opcode::LoadBool, static_cast<std::int64_t>(present)));
      }
    }
  }
  if (NeedsConstructorInitialization(owner)) {
    EmitConstructorInitialization(owner, object, arguments, location);
  }
}

void Lowerer::EmitConstructorInitialization(const frontend::ClassDescriptor& info,
                                            RegisterId object,
                                            std::span<const RegisterId> arguments,
                                            frontend::SourceLocation location) {
  const auto target = ConstructorTarget(info);
  const auto count = arguments.size();
  const auto base = AllocateRegisterWindow(count + 1, location);
  Emit({.opcode = Opcode::Move, .dst = base, .lhs = object, .location = location});
  for (std::size_t index = 0; index < count; ++index) {
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + index + 1),
        .lhs = arguments[index],
        .location = location,
    });
  }
  const auto ignored = AllocateRegister(location);
  Emit({
      .opcode = Opcode::Call,
      .dst = ignored,
      .lhs = base,
      .index = target,
      .location = location,
  });
}

}  // namespace vela::ir
