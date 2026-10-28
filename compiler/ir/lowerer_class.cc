#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

ClassId Lowerer::RegisterEnumType(const frontend::TypeDescriptor& type) {
  const auto name = type.nullable ? type.name.substr(0, type.name.size() - 1) : type.name;
  if (const auto existing = class_ids_.find(name); existing != class_ids_.end()) {
    return existing->second;
  }
  const auto index = static_cast<ClassId>(program_.classes.size());
  Class layout;
  layout.name = name;
  layout.kind = Class::Kind::Enum;
  layout.field_order = {"$value"};
  layout.field_types = {{.kind = frontend::TypeDescriptor::Kind::Int, .name = "int"}};
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
    if (type.kind == frontend::TypeDescriptor::Kind::Expected) {
      static_cast<void>(RegisterExpectedType(type));
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
    return LowerConversion(std::move(value), target, expression.location);
  }
  if (value.type.kind == frontend::TypeDescriptor::Kind::Enum) {
    value = LowerConversion(std::move(value),
                            {.kind = frontend::TypeDescriptor::Kind::Dynamic, .name = "dynamic"},
                            expression.location);
  }
  const auto index = RegisterRuntimeType(target);
  const auto result = AllocateTemporary(expression.location);
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
  current_function_ = &function;
  binding_registers_.clear();
  next_register_ = 0;
  scopes_.clear();
  controls_.clear();
  finalizers_.clear();
  current_handler_.reset();
  double_constant_ids_.clear();
  const auto value = LowerExpression(initializer);
  Emit({.opcode = Opcode::Return, .dst = value.reg, .location = location});
  function.register_count = next_register_;
  StoreFunction(std::move(function));
  current_function_ = nullptr;
}

Lowerer::LoweredValue Lowerer::LowerConstruction(const frontend::Expression& expression,
                                                 const frontend::TypeDescriptor& expected) {
  const auto& info = model_.classes.at(expected.name);
  const auto object = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::NewInstance,
      .dst = object,
      .index = class_ids_.at(expected.name),
      .location = expression.location,
  });
  return LowerConstructorCall(expression.arguments, model_.argument_order.at(&expression),
                              expression.location, info, object);
}

Lowerer::LoweredValue Lowerer::LowerConstructorCall(
    std::span<const frontend::CallArgument> call_arguments,
    std::span<const std::uint32_t> argument_order,
    frontend::SourceLocation location,
    const frontend::ClassDescriptor& info,
    RegisterId object) {
  const auto& source = *info.declaration;
  const auto& signature = info.constructor;
  const bool generated = source.constructor_kind == frontend::ConstructorKind::Generated;
  std::vector<RegisterId> arguments(generated ? 0 : signature.parameters.size());
  std::vector<bool> provided(info.field_order.size(), false);
  // param 实参依源码顺序直接写入尚未逸出的对象，避免为全部字段保留寄存器。
  for (std::size_t index = 0; index < call_arguments.size(); ++index) {
    const auto& argument = call_arguments.subspan(index).front();
    const auto parameter = argument_order.subspan(index).front();
    const auto value = LowerExpression(*argument.value);
    if (generated) {
      const auto field = info.fields.at(signature.parameters.at(parameter).name).index;
      Emit({
          .opcode = Opcode::SetField,
          .dst = object,
          .lhs = value.reg,
          .index = field,
          .location = argument.location,
      });
      provided.at(field) = true;
    } else {
      const auto saved = AllocateTemporary(argument.location);
      Emit({.opcode = Opcode::Move, .dst = saved, .lhs = value.reg, .location = argument.location});
      arguments.at(parameter) = saved;
    }
  }
  for (const auto& field : source.fields) {
    if (field.storage == frontend::MemberStorage::Static || !field.initializer) {
      continue;
    }
    const auto field_index = info.fields.at(field.name).index;
    if (provided.at(field_index)) {
      continue;
    }
    // 字段初始化器在声明处检查，不可意外读取构造调用者的同名局部变量。
    auto caller_scopes = std::move(scopes_);
    scopes_.clear();
    const auto value = LowerExpression(*field.initializer);
    scopes_ = std::move(caller_scopes);
    Emit({
        .opcode = Opcode::SetField,
        .dst = object,
        .lhs = value.reg,
        .index = field_index,
        .location = field.location,
    });
  }
  if (source.base_class && !model_.explicit_super_constructors.contains(info.type.name)) {
    const auto& base = model_.classes.at(source.base_class->name);
    static_cast<void>(LowerConstructorCall({}, {}, location, base, object));
  }
  if (source.constructor) {
    const auto count =
        source.constructor_kind == frontend::ConstructorKind::Explicit ? arguments.size() : 0;
    const auto base = next_register_;
    for (std::size_t index = 0; index <= count; ++index) {
      static_cast<void>(AllocateRegister(location));
    }
    Emit({.opcode = Opcode::Move, .dst = base, .lhs = object, .location = location});
    for (std::size_t index = 0; index < count; ++index) {
      Emit({
          .opcode = Opcode::Move,
          .dst = static_cast<RegisterId>(base + index + 1),
          .lhs = arguments.at(index),
          .location = location,
      });
    }
    const auto ignored = AllocateTemporary(location);
    Emit({
        .opcode = Opcode::Call,
        .dst = ignored,
        .lhs = base,
        .index = function_ids_.at(info.type.name + ".$constructor"),
        .location = location,
    });
  }
  return {.reg = object, .type = info.type};
}

}  // namespace vela::ir
