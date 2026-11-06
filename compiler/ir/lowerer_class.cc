#include "base/declaration.h"
#include "base/layout_description.h"
#include "base/native_binding.h"
#include "base/native_program_mode.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/lowerer.h"

#include <algorithm>
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
      LowerInitializer(InitializerKey(InitializerKind::StaticField, owner.name + '.' + field.name),
                       *field.initializer, info.static_fields.at(field.name).type, field.location);
    } else if (native_mode_ == NativeProgramMode::Reloadable && field.initializer != nullptr) {
      LowerInitializer(
          InitializerKey(InitializerKind::InstanceField, owner.name + '.' + field.name),
          *field.initializer, info.fields.at(field.name).type, field.location);
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
      std::ranges::any_of(source.fields,
                          [](const auto& field) {
                            return field.storage == frontend::MemberStorage::Instance &&
                                   field.initializer;
                          }) ||
      (owner.base_class && HasInstanceInitialization(model_.classes.at(*owner.base_class)));
  instance_initialization_.emplace(owner.type.name, initialized);
  return initialized;
}

std::uint32_t Lowerer::ConstructorMaskCount(const frontend::ClassDescriptor& owner) const {
  if (native_mode_ == NativeProgramMode::Reloadable) {
    return 0;
  }
  const auto& source = *owner.declaration;
  const bool has_optional_fields =
      source.constructor_kind == frontend::ConstructorKind::Generated &&
      !owner.constructor.optional_parameters.empty();
  return has_optional_fields
             ? static_cast<std::uint32_t>((owner.field_order.size() + kConstructorMaskBits - 1) /
                                          kConstructorMaskBits)
             : 0;
}

void Lowerer::LowerConstructorInitializers(const frontend::ClassDescriptor& owner) {
  const auto& source = *owner.declaration;
  for (const auto& field : source.fields) {
    if (field.storage == frontend::MemberStorage::Static || !field.initializer) {
      continue;
    }
    const auto index = owner.fields.at(field.name).index;
    if (native_mode_ == NativeProgramMode::Reloadable &&
        source.constructor_kind == frontend::ConstructorKind::Generated) {
      // 显式实参已按稳定字段身份写入对象；读取只为尚未提供的槽运行当前初始化器。
      const auto value = EmitTemporary({
          .opcode = Opcode::GetField,
          .lhs = 0,
          .index = index,
          .location = field.location,
          .field_declaration = FieldDeclaration(owner.type, index),
      });
      RecordValueType(value, owner.fields.at(field.name).type);
      continue;
    }
    std::optional<std::size_t> provided;
    if (source.IsConstructorField(field)) {
      const auto mask = EmitTemporary({
          .opcode = Opcode::LoadInt,
          .immediate =
              std::bit_cast<std::int64_t>(std::uint64_t{1} << (index % kConstructorMaskBits)),
          .location = field.location,
      });
      const auto bits = EmitTemporary({
          .opcode = Opcode::BitAndInt,
          .lhs = 1 + (index / kConstructorMaskBits),
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
  if (source.base_class && !model_.explicit_super_constructors.contains(owner.type.name)) {
    const auto& base = model_.classes.at(source.base_class->name);
    static_cast<void>(LowerConstructorCall({}, {}, source.location, base, 0));
  }
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
  if (declarations_ != nullptr) {
    layout.declaration = declarations_->nominals.at(name);
  }
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
    const auto& fields = declarations_->classes.at(expected.name);
    const auto generated =
        info.declaration->constructor_kind == frontend::ConstructorKind::Generated;
    construction.emplace(
        Construction{.type = fields.declaration, .supplied = {}, .generated = generated});
    if (generated) {
      for (const auto parameter : model_.argument_order.at(&expression)) {
        construction->supplied.push_back(
            fields.fields.at(info.constructor.parameters.at(parameter).name));
      }
    }
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

Lowerer::LoweredValue Lowerer::LowerConstructorCall(
    std::span<const frontend::CallArgument> call_arguments,
    std::span<const std::uint32_t> argument_order,
    frontend::SourceLocation location,
    const frontend::ClassDescriptor& info,
    RegisterId object,
    std::span<const LoweredValue> evaluated) {
  const auto& source = *info.declaration;
  const auto& signature = info.constructor;
  const bool generated = source.constructor_kind == frontend::ConstructorKind::Generated;
  const auto masks = ConstructorMaskCount(info);
  std::vector<RegisterId> arguments(generated ? 0 : signature.parameters.size());
  std::vector<std::uint64_t> provided(masks, 0);
  // 自动构造的实参依源码顺序直接写入尚未逸出的对象，避免为全部字段保留寄存器。
  for (std::size_t index = 0; index < call_arguments.size(); ++index) {
    const auto& argument = call_arguments[index];
    const auto parameter = argument_order[index];
    const auto value = evaluated.empty() ? LowerExpression(*argument.value) : evaluated[index];
    if (generated) {
      const auto field = info.fields.at(signature.parameters.at(parameter).name).index;
      Emit({
          .opcode = Opcode::SetField,
          .dst = object,
          .lhs = value.reg,
          .index = field,
          .location = argument.location,
          .field_declaration = FieldDeclaration(info.type, field),
      });
      if (masks != 0) {
        provided.at(field / kConstructorMaskBits) |= std::uint64_t{1}
                                                     << (field % kConstructorMaskBits);
      }
    } else {
      const auto saved = AllocateRegister(argument.location);
      Emit({.opcode = Opcode::Move, .dst = saved, .lhs = value.reg, .location = argument.location});
      arguments.at(parameter) = saved;
    }
  }
  if (HasInstanceInitialization(info) ||
      (generated && native_mode_ == NativeProgramMode::Reloadable)) {
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
