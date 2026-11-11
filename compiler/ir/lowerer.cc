#include "compiler/ir/lowerer.h"
#include "base/declaration.h"
#include "base/member.h"
#include "compiler/frontend/syntax/names.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "base/builtin.h"
#include "base/callable.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

using frontend::TokenKind;
namespace {

bool IsFutureFactoryCall(const frontend::Expression& expression,
                         const frontend::SemanticModel& model) {
  using Expression = frontend::Expression;
  if (expression.kind != Expression::Kind::Call) {
    return false;
  }
  const auto& callee = *expression.callee;
  const auto& receiver = callee.object;
  return callee.kind == Expression::Kind::Member && receiver->kind == Expression::Kind::Name &&
         receiver->text == frontend::kFutureTypeName && !model.bindings.contains(receiver.get());
}

Opcode SelectIntegerOpcode(TokenKind operation) {
  switch (operation) {
    case TokenKind::Plus:
      return Opcode::AddInt;
    case TokenKind::Minus:
      return Opcode::SubInt;
    case TokenKind::Star:
      return Opcode::MulInt;
    case TokenKind::Slash:
      return Opcode::DivInt;
    case TokenKind::Percent:
      return Opcode::ModInt;
    case TokenKind::Ampersand:
      return Opcode::BitAndInt;
    case TokenKind::Pipe:
      return Opcode::BitOrInt;
    case TokenKind::Caret:
      return Opcode::BitXorInt;
    case TokenKind::ShiftLeft:
      return Opcode::ShiftLeftInt;
    case TokenKind::ShiftRight:
      return Opcode::ShiftRightInt;
    case TokenKind::ShiftUnsignedRight:
      return Opcode::ShiftUnsignedRightInt;
    case TokenKind::EqualEqual:
      return Opcode::EqualInt;
    case TokenKind::BangEqual:
      return Opcode::NotEqualInt;
    case TokenKind::Less:
      return Opcode::LessInt;
    case TokenKind::LessEqual:
      return Opcode::LessEqualInt;
    case TokenKind::Greater:
      return Opcode::GreaterInt;
    case TokenKind::GreaterEqual:
      return Opcode::GreaterEqualInt;
    default:
      std::unreachable();
  }
}

}  // namespace

Opcode Lowerer::SelectBinaryOpcode(TokenKind operation, const frontend::TypeDescriptor& type) {
  using Kind = frontend::TypeDescriptor::Kind;
  if (type.kind == Kind::Double) {
    return SelectDoubleOpcode(operation);
  }
  if (type.kind == Kind::String) {
    if (operation == TokenKind::Plus) {
      return Opcode::StringConcat;
    }
    if (operation == TokenKind::EqualEqual) {
      return Opcode::EqualString;
    }
    if (operation == TokenKind::BangEqual) {
      return Opcode::NotEqualString;
    }
    std::unreachable();
  }
  if (type.kind == Kind::Bool) {
    if (operation == TokenKind::EqualEqual) {
      return Opcode::EqualBool;
    }
    if (operation == TokenKind::BangEqual) {
      return Opcode::NotEqualBool;
    }
    std::unreachable();
  }
  return SelectIntegerOpcode(operation);
}

Opcode Lowerer::SelectEqualityOpcode(const frontend::TypeDescriptor& type) {
  using Kind = frontend::TypeDescriptor::Kind;
  if (!type.nullable) {
    switch (type.kind) {
      case Kind::Int:
      case Kind::Enum:
        return Opcode::EqualInt;
      case Kind::Bool:
        return Opcode::EqualBool;
      case Kind::String:
        return Opcode::EqualString;
      default:
        break;
    }
  }
  return Opcode::EqualValue;
}

LoweringResult Lowerer::Lower() && {
  program_.native_mode = native_mode_;
  BuildFunctionTable();
  BuildClassTable();
  for (const auto& enumeration : source_.enums) {
    static_cast<void>(RegisterRuntimeType(model_.types.at(enumeration.name)));
  }
  LowerExposedConstructors();
  LowerJSONDecodePlans();
  for (const auto& [name, selector] : interface_method_ids_) {
    if (program_.selector_declarations.size() <= selector) {
      program_.selector_declarations.resize(static_cast<std::size_t>(selector) + 1);
    }
    program_.selector_declarations.at(selector) = declarations_->selectors.at(name);
  }
  if (!model_.captured_mutables.empty()) {
    class_ids_.emplace("$Cell", static_cast<ClassId>(program_.classes.size()));
    program_.classes.push_back({
        .name = "$Cell",
        .field_types = {{.kind = frontend::TypeDescriptor::Kind::Class, .name = "$Cell"}},
        .callable = std::nullopt,
        .interfaces = {},
        .dispatch_targets = {},
        .kind = Class::Kind::CaptureCell,
        .native_key = {},
        .native_contract = {},
        .callable_type = {},
        .declaration = {},
        .fields = {},
        .base_class = {},
        .enum_members = {},
    });
  }
  for (const frontend::Function& function : source_.functions) {
    LowerFunction(function, model_.functions.at(function.name));
  }
  for (const frontend::Class& owner : source_.classes) {
    const auto& descriptor = model_.classes.at(owner.name);
    for (const frontend::Function& method : owner.methods) {
      LowerFunction(method, descriptor.methods.at(method.name), &owner);
    }
  }
  for (const auto& owner : source_.classes) {
    LowerFieldDefaultInitializers(owner);
  }
  for (const auto& global : source_.globals) {
    LowerInitializer(InitializerKey(InitializerKind::Global, global.name), *global.initializer,
                     model_.globals.at(global.name).type, global.location);
  }
  for (const auto& lambda : model_.lambdas) {
    LowerFunction(*lambda.expression->function, lambda.signature, nullptr, &lambda);
  }
  for (const auto& generated : model_.generated_functions) {
    LowerFunction(*generated.declaration, generated.signature);
  }
  // 降低构造时还会登记父类构造；按索引消费增长中的工作表，不借用可能失效的迭代器。
  std::size_t constructor_index = 0;
  while (constructor_index < pending_constructors_.size()) {
    const auto& owner = *pending_constructors_.at(constructor_index);
    ++constructor_index;
    const auto& descriptor = model_.classes.at(owner.name);
    if (owner.constructor) {
      LowerFunction(*owner.constructor, descriptor.constructor, &owner);
    } else {
      frontend::Function constructor;
      constructor.name = frontend::kConstructorName;
      constructor.location = owner.location;
      LowerFunction(constructor, descriptor.constructor, &owner);
    }
  }
  RegisterBuiltinMembers();
  ResolveCallValueTypes();
  LowerReflection();
  BuildNativeClassContracts();
  return {.program = std::move(program_), .diagnostics = std::move(diagnostics_)};
}

void Lowerer::AddDiagnostic(frontend::SourceLocation location, std::string message) {
  diagnostics_.push_back({
      .location = location,
      .message = std::move(message),
      .source_path =
          source_.sources.empty() ? std::string() : source_.sources.at(location.source_id).path,
  });
}

void Lowerer::BuildInterfaceSelectors() {
  for (const auto& interface : source_.interfaces) {
    for (const auto& method : interface.methods) {
      const auto key = QualifiedMemberKey({.owner = interface.name, .name = method.name});
      const auto selector = static_cast<std::uint32_t>(interface_method_ids_.size());
      interface_method_ids_.emplace(key, selector);
    }
  }
}

void Lowerer::BuildClassTable() {
  BuildInterfaceSelectors();
  const auto first_class = program_.classes.size();
  for (const frontend::Class& source_class : source_.classes) {
    const auto class_id = static_cast<ClassId>(program_.classes.size());
    class_ids_.emplace(source_class.name, class_id);
    program_.classes.emplace_back();
  }
  for (std::size_t index = 0; index < source_.classes.size(); ++index) {
    const auto& source_class = source_.classes[index];
    const auto& class_info = model_.classes.at(source_class.name);
    auto descriptor = BuildClass(source_class, class_info);
    program_.classes.at(first_class + index) = std::move(descriptor);
    RegisterClassMembers(class_info);
  }
  for (const auto& source : source_.interfaces) {
    const auto& descriptor = model_.interfaces.at(source.name);
    const auto receiver = RegisterRuntimeType(descriptor.type);
    for (const auto& method : source.methods) {
      const auto& signature = descriptor.methods.at(method.name);
      const std::array captures{MakeReceiverCapture(descriptor.type)};
      const auto wrapper = CallableClass(
          interface_method_ids_.at(QualifiedMemberKey({.owner = source.name, .name = method.name})),
          CallableMode::InterfaceMethod, captures, &signature);
      program_.members.push_back({
          .receiver_type = receiver,
          .name = method.name,
          .kind = MemberKind::Method,
          .target = wrapper,
      });
    }
  }
}

Class Lowerer::BuildClass(const frontend::Class& source_class,
                          const frontend::ClassDescriptor& class_info) {
  Class output_class;
  output_class.name = source_class.name;
  output_class.native_key = source_class.native_key;
  const auto& source_path = source_.sources.at(source_class.location.source_id).path;
  switch (source_class.reflection_role) {
    case frontend::Class::ReflectionRole::None:
      break;
    case frontend::Class::ReflectionRole::Metadata:
      output_class.kind = Class::Kind::Metadata;
      break;
    case frontend::Class::ReflectionRole::Identity:
      output_class.kind = Class::Kind::ReflectionIdentity;
      break;
  }
  if (!source_class.native_key.empty()) {
    output_class.kind = Class::Kind::Host;
  }
  if (source_path == frontend::kErrorSourceName) {
    output_class.kind = Class::Kind::RuntimeError;
  }
  if (source_path == "builtin:DateParts") {
    output_class.kind = Class::Kind::DateParts;
  }
  if (!class_info.interfaces.empty()) {
    output_class.dispatch_targets.resize(interface_method_ids_.size(), kInvalidFunctionTarget);
  }
  for (const auto& name : class_info.interfaces) {
    const auto& interface = model_.interfaces.at(name);
    output_class.interfaces.push_back(interface.index);
  }
  // 语义层使用集合查询实现关系；输出按运行时编号规范化，避免哈希顺序改变原生缓存键。
  std::ranges::sort(output_class.interfaces);
  for (const auto& [signature, target] : class_info.interface_targets) {
    const auto selected = interface_method_ids_.find(
        QualifiedMemberKey({.owner = signature.first, .name = signature.second}));
    if (selected != interface_method_ids_.end()) {
      output_class.dispatch_targets.at(selected->second) =
          function_ids_.at(QualifiedMemberKey({.owner = target.owner, .name = target.member}));
    }
  }
  const auto& declarations = declarations_->classes.at(source_class.name);
  output_class.declaration = declarations.declaration;
  if (class_info.base_class) {
    output_class.base_class = declarations_->nominals.at(*class_info.base_class);
  }
  output_class.field_types.reserve(class_info.field_order.size());
  for (const auto& name : class_info.field_order) {
    const auto identity = declarations.fields.at(name);
    const auto& field = class_info.fields.at(name);
    output_class.field_types.push_back(field.type);
    const auto initializer = field_initializers_.find(identity);
    output_class.fields.push_back({
        .declaration = identity,
        .type = RegisterRuntimeType(field.type),
        .visibility = field.visibility,
        .initializer =
            initializer == field_initializers_.end() ? kInvalidFunctionTarget : initializer->second,
    });
  }
  return output_class;
}

std::uint32_t Lowerer::AddFunctionId(const std::string& name) {
  const auto target = static_cast<std::uint32_t>(function_ids_.size());
  function_ids_.emplace(name, target);
  program_.functions.resize(function_ids_.size());
  return target;
}

void Lowerer::AddGlobalId(const std::string& name, InitializerKind kind) {
  const auto slot = static_cast<std::uint32_t>(program_.global_initializers.size());
  global_ids_.emplace(name, slot);
  const auto initializer = InitializerKey(kind, name);
  const auto function = AddFunctionId(initializer);
  program_.global_initializers.push_back(function);
}

void Lowerer::BuildFunctionTable() {
  for (const auto& function : source_.functions) {
    static_cast<void>(AddFunctionId(function.name));
  }
  for (const frontend::Class& owner : source_.classes) {
    for (const frontend::Function& method : owner.methods) {
      const auto name = MethodKey(owner.name, method);
      static_cast<void>(AddFunctionId(name));
    }
    const auto name = MethodKey(owner.name, DeclarationKind::Method, frontend::kConstructorName);
    if (owner.constructor_kind != frontend::ConstructorKind::Unavailable &&
        native_mode_ == NativeProgramMode::Reloadable) {
      static_cast<void>(AddFunctionId(name));
      pending_constructors_.push_back(&owner);
    }
  }
  for (const auto& owner : source_.classes) {
    for (const auto& field : owner.fields) {
      if (field.storage == frontend::MemberStorage::Static) {
        const auto key = QualifiedMemberKey({.owner = owner.name, .name = field.name});
        AddGlobalId(key, InitializerKind::StaticField);
      } else if (native_mode_ == NativeProgramMode::Reloadable && field.initializer != nullptr) {
        const auto target = AddFunctionId(
            InitializerKey(InitializerKind::InstanceField,
                           QualifiedMemberKey({.owner = owner.name, .name = field.name})));
        field_initializers_.emplace(declarations_->classes.at(owner.name).fields.at(field.name),
                                    target);
      }
    }
  }
  for (const auto& global : source_.globals) {
    AddGlobalId(global.name, InitializerKind::Global);
  }
  lambda_ids_.clear();
  for (const auto& generated : model_.generated_functions) {
    static_cast<void>(AddFunctionId(generated.declaration->name));
  }
  for (std::size_t index = 0; index < model_.lambdas.size(); ++index) {
    const auto target = static_cast<std::uint32_t>(function_ids_.size());
    lambda_ids_.push_back(target);
    function_ids_.emplace("$lambda" + std::to_string(target), target);
  }
  program_.functions.resize(function_ids_.size());
}

void Lowerer::BeginFunctionBody(Function& function, const frontend::Class* owner) {
  current_function_ = &function;
  current_owner_ = owner;
  binding_registers_.clear();
  next_register_ = 0;
  double_constant_ids_.clear();
  scopes_.clear();
  controls_.clear();
  finalizers_.clear();
  current_handler_.reset();
}

void Lowerer::RecordParameterValueTypes(Function& function) {
  function.value_types.resize(function.parameter_count);
  if (function.signature) {
    const auto& signature = *function.signature;
    if (signature.receiver) {
      function.value_types.front() = program_.runtime_types.at(*signature.receiver);
    }
    for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
      const auto& parameter = signature.parameters.at(index);
      auto type = program_.runtime_types.at(parameter.type);
      if (parameter.optional) {
        type = frontend::MakeNullableType(type);
      }
      function.value_types.at(index + signature.hidden_parameters) = type;
    }
  }
}

void Lowerer::StoreFunction(Function function) {
  function.value_types.resize(function.register_count);
  const auto target = function_ids_.at(function.name);
  if (function.identity.revision == 0) {
    const auto named = declarations_->named_functions.find(function.name);
    function.identity.revision = declarations_->revision;
    if (named != declarations_->named_functions.end()) {
      function.identity.declaration = named->second;
    } else {
      function.identity.local = target + 1;
    }
  }
  program_.functions.at(target) = std::move(function);
}

void Lowerer::LowerCaptures(const frontend::Function& source,
                            const frontend::LambdaDescriptor& lambda) {
  for (std::size_t index = 0; index < lambda.captures.size(); ++index) {
    const auto& capture = lambda.captures.at(index);
    const auto reg = AllocateRegister(source.location);
    RecordValueType(reg,
                    capture.kind == frontend::BindingKind::Mutable
                        ? frontend::TypeDescriptor{.kind = frontend::TypeDescriptor::Kind::Class,
                                                   .name = "$Cell",}
                        : capture.type);
    Emit({
        .opcode = Opcode::GetField,
        .dst = reg,
        .lhs = 0,
        .index = static_cast<FieldId>(index),
        .location = source.location,
    });
    scopes_.back().emplace(capture.name, Local{
                                             .reg = reg,
                                             .type = capture.type,
                                             .cell = capture.kind == frontend::BindingKind::Mutable,
                                         });
    binding_registers_.emplace(capture.binding, reg);
  }
}

void Lowerer::LowerFunctionParameters(const frontend::Function& source,
                                      const frontend::FunctionDescriptor& function_info,
                                      const frontend::Class* owner,
                                      const frontend::LambdaDescriptor* lambda) {
  const bool has_receiver = owner != nullptr && source.storage == frontend::MemberStorage::Instance;
  const auto* constructor = owner != nullptr && source.name == frontend::kConstructorName
                                ? &model_.classes.at(owner->name)
                                : nullptr;
  const auto parameter_base =
      current_function_->parameter_count - static_cast<std::uint32_t>(source.parameters.size());
  const auto masks = parameter_base - static_cast<std::uint32_t>(has_receiver || lambda != nullptr);
  if (lambda != nullptr) {
    static_cast<void>(AllocateRegister(source.location));
    RecordValueType(0, model_.expression_types.at(lambda->expression));
  }

  if (has_receiver) {
    const RegisterId this_reg = AllocateRegister(source.location);
    RecordValueType(this_reg, model_.classes.at(owner->name).type);
  }
  for (std::uint32_t index = 0; index < masks; ++index) {
    const auto reg = AllocateRegister(source.location);
    RecordValueType(reg,
                    {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
  }

  for (const frontend::Parameter& parameter : source.parameters) {
    static_cast<void>(AllocateRegister(parameter.location));
  }
  if (constructor != nullptr) {
    // 初始化器只看声明处的全局环境；构造参数与 this 尚未发布到词法作用域。
    LowerConstructorInitializers(*constructor);
  }
  if (has_receiver) {
    DeclareLocal(frontend::kReceiverName, 0, model_.classes.at(owner->name).type, source.location,
                 source.body.get());
  }
  // 完整参数窗口建立后，再为可捕获的参数分配共享槽位。
  for (std::size_t index = 0; index < source.parameters.size(); ++index) {
    const auto& parameter = source.parameters.at(index);
    const auto reg = static_cast<RegisterId>(index) + parameter_base;
    DeclareLocal(
        parameter.name, reg,
        function_info.parameters.at(function_info.parameter_indices.at(parameter.name)).type,
        parameter.location, &parameter);
  }
  if (lambda != nullptr) {
    LowerCaptures(source, *lambda);
  }
}

CallSignature::Parameter Lowerer::DescribeCallParameter(
    const frontend::FunctionDescriptor& source,
    const frontend::FunctionDescriptor::Parameter& parameter) {
  return {
      .name = parameter.name,
      .type = RegisterRuntimeType(parameter.type),
      .optional = source.optional_parameters.contains(parameter.name),
      .rest = parameter.rest,
  };
}

CallSignature Lowerer::DescribeCallSignature(const frontend::FunctionDescriptor& source) {
  CallSignature signature;
  for (const auto& parameter : source.parameters) {
    signature.parameters.push_back(DescribeCallParameter(source, parameter));
  }
  if (source.return_type.kind != frontend::TypeDescriptor::Kind::Void) {
    signature.result = RegisterRuntimeType(source.return_type);
  }
  return signature;
}

CallSignature& Lowerer::RegisterFunctionSignature(
    Function& function,
    const frontend::Function& source,
    const frontend::FunctionDescriptor& function_info) {
  auto& signature = function.signature.emplace();
  signature.hidden_parameters =
      static_cast<std::uint8_t>(function.parameter_count - source.parameters.size());
  for (const auto& parameter : source.parameters) {
    const auto& descriptor =
        function_info.parameters.at(function_info.parameter_indices.at(parameter.name));
    signature.parameters.push_back(DescribeCallParameter(function_info, descriptor));
  }
  if (function.return_type.kind != frontend::TypeDescriptor::Kind::Void) {
    signature.result = RegisterRuntimeType(function.return_type);
  }
  return signature;
}

Function Lowerer::PrepareFunction(const frontend::Function& source,
                                  const frontend::FunctionDescriptor& function_info,
                                  const frontend::Class* owner,
                                  const frontend::LambdaDescriptor* lambda) {
  Function function;
  function.is_generated_constructor =
      owner != nullptr && source.name == frontend::kConstructorName &&
      owner->constructor_kind == frontend::ConstructorKind::Generated;
  function.location = source.location;
  const auto identity = declarations_->functions.find(&source);
  if (identity != declarations_->functions.end()) {
    function.identity = identity->second;
  }
  function.is_async = source.is_async;
  function.native_symbol = source.native_symbol;
  function.visibility = source.visibility;
  function.is_exported =
      owner == nullptr && lambda == nullptr &&
      (native_mode_ == NativeProgramMode::Reloadable || source.location.source_id == 0) &&
      source.export_kind == frontend::ExportKind::Exported;
  if (lambda != nullptr) {
    function.name =
        "$lambda" + std::to_string(lambda_ids_.at(model_.lambda_indices.at(lambda->expression)));
  } else {
    function.name = owner == nullptr ? source.name : MethodKey(owner->name, source);
  }
  function.return_type = function_info.return_type;
  if (source.is_setter && function_info.setter_return_type) {
    function.return_type = *function_info.setter_return_type;
  }
  if (owner != nullptr && source.name == frontend::kConstructorName && owner->native_key.empty()) {
    function.return_type = {
        .kind = frontend::TypeDescriptor::Kind::Void,
        .name = frontend::kVoidTypeName,
    };
  }
  const bool has_receiver = owner != nullptr && source.storage == frontend::MemberStorage::Instance;
  const auto masks = owner != nullptr && source.name == frontend::kConstructorName
                         ? ConstructorMaskCount(model_.classes.at(owner->name))
                         : 0;
  function.parameter_count = static_cast<std::uint32_t>(
      source.parameters.size() + ((has_receiver || lambda != nullptr) ? 1 : 0) + masks);
  auto& signature = RegisterFunctionSignature(function, source, function_info);
  if (has_receiver) {
    signature.receiver = RegisterRuntimeType(model_.classes.at(owner->name).type);
  }
  if (masks != 0) {
    signature.hidden_parameters = 1;
    const auto integer = RegisterRuntimeType(
        {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
    for (std::uint32_t index = 0; index < masks; ++index) {
      signature.parameters.push_back(
          {.name = "$provided" + std::to_string(index), .type = integer});
    }
  }
  RecordParameterValueTypes(function);
  return function;
}

void Lowerer::LowerFunction(const frontend::Function& source,
                            const frontend::FunctionDescriptor& function_info,
                            const frontend::Class* owner,
                            const frontend::LambdaDescriptor* lambda) {
  auto function = PrepareFunction(source, function_info, owner, lambda);
  if (source.intrinsic) {
    LowerIntrinsicFunction(std::move(function), source, owner, *source.intrinsic);
    return;
  }
  if (!source.native_symbol.empty()) {
    function.register_count = std::max(function.parameter_count, 1U);
    StoreFunction(std::move(function));
    return;
  }
  BeginFunctionBody(function, owner);
  PushScope();

  LowerFunctionParameters(source, function_info, owner, lambda);

  if (source.body != nullptr) {
    for (const auto& child : source.body->statements) {
      LowerStatement(*child);
    }
  }
  PopScope();

  bool returns_void = function.return_type.kind == frontend::TypeDescriptor::Kind::Void;
  if (source.is_async && function.return_type.kind == frontend::TypeDescriptor::Kind::Future) {
    const auto arguments = model_.generic_arguments.find(function.return_type.name);
    returns_void = arguments != model_.generic_arguments.end() && arguments->second.size() == 1 &&
                   arguments->second.front().kind == frontend::TypeDescriptor::Kind::Void;
  }
  if (returns_void &&
      (source.body == nullptr || model_.control_flow.at(source.body.get()).can_fall_through)) {
    const RegisterId result = AllocateRegister(source.location);
    Emit({.opcode = Opcode::LoadNull, .dst = result, .location = source.location});
    Emit({.opcode = Opcode::Return, .dst = result, .location = source.location});
  }
  function.register_count = next_register_;

  StoreFunction(std::move(function));
  current_function_ = nullptr;
}

void Lowerer::LowerStatement(const frontend::Statement& statement) {
  switch (statement.kind) {
    case frontend::Statement::Kind::VariableDeclaration:
      LowerVariableDeclaration(statement);
      break;
    case frontend::Statement::Kind::Return:
      LowerReturn(statement);
      break;
    case frontend::Statement::Kind::Throw: {
      const auto value = LowerExpression(*statement.expression);
      Emit({.opcode = Opcode::Throw, .lhs = value.reg, .location = statement.location});
      break;
    }
    case frontend::Statement::Kind::Try:
      LowerTry(statement);
      break;
    case frontend::Statement::Kind::Expression:
      if (statement.expression != nullptr) {
        const auto value = LowerExpression(*statement.expression);
        if (statement.echo && value.type.kind != frontend::TypeDescriptor::Kind::Void) {
          Emit({.opcode = Opcode::Print, .lhs = value.reg, .location = statement.location});
        }
      }
      break;
    case frontend::Statement::Kind::Block:
      LowerBlock(statement);
      break;
    case frontend::Statement::Kind::If:
      LowerIf(statement);
      break;
    case frontend::Statement::Kind::While:
      LowerWhile(statement);
      break;
    case frontend::Statement::Kind::For:
      LowerFor(statement);
      break;
    case frontend::Statement::Kind::ForEach:
      LowerForEach(statement);
      break;
    case frontend::Statement::Kind::Switch:
      LowerSwitch(statement);
      break;
    case frontend::Statement::Kind::Break:
    case frontend::Statement::Kind::Continue:
      LowerControlExit(statement);
      break;
  }
}

void Lowerer::LowerBlock(const frontend::Statement& statement) {
  PushScope();
  for (const std::unique_ptr<frontend::Statement>& child : statement.statements) {
    LowerStatement(*child);
  }
  PopScope();
}

Lowerer::LoweredValue Lowerer::LowerExpression(const frontend::Expression& expression) {
  const auto first_temporary = next_register_;
  auto value = LowerExpressionValue(expression);
  const auto record_definition = [&] {
    if (value.reg >= first_temporary) {
      RecordValueType(value.reg, value.type, ValueTypeRecording::Declare);
    }
  };
  record_definition();
  using Kind = frontend::TypeDescriptor::Kind;
  using Expression = frontend::Expression;
  if ((value.type.kind == Kind::Future || value.type.kind == Kind::Completer) &&
      (expression.kind == Expression::Kind::Call || expression.kind == Expression::Kind::Member ||
       expression.kind == Expression::Kind::Await)) {
    const auto type = RegisterRuntimeType(value.type);
    const auto identity = EmitTemporary(
        {.opcode = Opcode::LoadInt, .immediate = type, .location = expression.location});
    const auto result = AllocateRegister(expression.location);
    const std::array arguments{value.reg, identity};
    EmitBuiltinCall(Builtin::BindRuntimeType, result, arguments, expression.location);
    value.reg = result;
  }
  const auto conversion = model_.checked_conversions.find(&expression);
  if (conversion != model_.checked_conversions.end()) {
    value = LowerConversion(value, conversion->second, expression.location);
  }
  record_definition();
  return value;
}

Lowerer::LoweredValue Lowerer::LowerConversion(LoweredValue value,
                                               const frontend::TypeDescriptor& target,
                                               frontend::SourceLocation location) {
  if (target.kind == frontend::TypeDescriptor::Kind::Dynamic) {
    if (value.type.kind != frontend::TypeDescriptor::Kind::Enum) {
      return {.reg = value.reg, .type = target};
    }
    const auto layout = RegisterEnumType(value.type);
    const auto boxed = AllocateRegister(location);
    std::optional<std::size_t> null_jump;
    if (value.type.nullable) {
      Emit({.opcode = Opcode::LoadNull, .dst = boxed, .location = location});
      null_jump = BeginNullSafe(value.reg, location);
    }
    Emit({.opcode = Opcode::NewInstance, .dst = boxed, .index = layout, .location = location});
    Emit({.opcode = Opcode::SetField, .dst = boxed, .lhs = value.reg, .location = location});
    if (null_jump) {
      BindJump(*null_jump);
    }
    return {.reg = boxed, .type = target};
  }
  if (value.type.kind == frontend::TypeDescriptor::Kind::Enum) {
    value = LowerConversion(
        value,
        {.kind = frontend::TypeDescriptor::Kind::Dynamic, .name = frontend::kDynamicTypeName},
        location);
  }
  const auto index = RegisterRuntimeType(target);
  auto result = AllocateRegister(location);
  Emit({
      .opcode = Opcode::CheckedCast,
      .dst = result,
      .lhs = value.reg,
      .index = index,
      .location = location,
  });
  if (target.kind == frontend::TypeDescriptor::Kind::Enum) {
    const auto boxed = result;
    result = AllocateRegister(location);
    RecordValueType(result, target);
    std::optional<std::size_t> null_jump;
    if (target.nullable) {
      Emit({.opcode = Opcode::LoadNull, .dst = result, .location = location});
      null_jump = BeginNullSafe(boxed, location);
    }
    Emit({.opcode = Opcode::GetField, .dst = result, .lhs = boxed, .location = location});
    if (null_jump) {
      BindJump(*null_jump);
    }
  }
  return {.reg = result, .type = target};
}

Lowerer::LoweredValue Lowerer::LowerExpressionValue(const frontend::Expression& expression) {
  using Kind = frontend::Expression::Kind;
  const auto kind = expression.kind;
  if (kind == Kind::Call) {
    if (model_.array_algorithms.contains(&expression)) {
      return LowerArrayAlgorithm(expression);
    }
    if (model_.collection_constructions.contains(&expression)) {
      return LowerCollectionConstruction(expression);
    }
  }
  if ((kind == Kind::Call || kind == Kind::Member || kind == Kind::Index) &&
      model_.builtin_calls.contains(&expression)) {
    return LowerBuiltin(expression);
  }
  if ((kind == Kind::Call || kind == Kind::Member) && model_.async_calls.contains(&expression)) {
    return LowerAsyncCall(expression);
  }
  switch (expression.kind) {
    case frontend::Expression::Kind::NullLiteral: {
      const auto reg = AllocateRegister(expression.location);
      Emit({.opcode = Opcode::LoadNull, .dst = reg, .location = expression.location});
      return {
          .reg = reg,
          .type = {.kind = frontend::TypeDescriptor::Kind::Null, .name = frontend::kNullTypeName},
      };
    }
    case frontend::Expression::Kind::IntegerLiteral:
      return LowerIntegerLiteral(expression);
    case frontend::Expression::Kind::BooleanLiteral: {
      const RegisterId reg = AllocateRegister(expression.location);
      Emit({
          .opcode = Opcode::LoadBool,
          .dst = reg,
          .immediate = expression.boolean_value ? 1 : 0,
          .location = expression.location,
      });
      return {
          .reg = reg,
          .type = {.kind = frontend::TypeDescriptor::Kind::Bool, .name = frontend::kBoolTypeName},
      };
    }
    case frontend::Expression::Kind::DoubleLiteral:
      return EmitDoubleConstant(expression.double_value, expression.location);
    case frontend::Expression::Kind::StringLiteral:
      return EmitStringConstant(expression.text, expression.location);
    case frontend::Expression::Kind::StringInterpolation:
      return LowerStringInterpolation(expression);
    case frontend::Expression::Kind::ArrayLiteral:
      return LowerArrayLiteral(expression);
    case frontend::Expression::Kind::MapLiteral:
      return LowerMapLiteral(expression);
    case frontend::Expression::Kind::Index:
      return LowerIndex(expression);
    case frontend::Expression::Kind::Name:
      return LowerName(expression);
    case frontend::Expression::Kind::Lambda:
      return LowerFunctionValue(expression);
    case frontend::Expression::Kind::TypeTest:
    case frontend::Expression::Kind::TypeCast:
      return LowerTypeOperation(expression);
    case frontend::Expression::Kind::Match:
      return LowerMatch(expression);
    case frontend::Expression::Kind::Await:
      return LowerAwait(expression);
    case frontend::Expression::Kind::Conditional:
      return LowerConditional(expression);
    case frontend::Expression::Kind::Unary:
      return LowerUnary(expression);
    case frontend::Expression::Kind::Binary:
      return LowerBinary(expression);
    case frontend::Expression::Kind::CompoundAssignment:
      return LowerCompoundAssignment(expression);
    case frontend::Expression::Kind::Call:
      return LowerCall(expression);
    case frontend::Expression::Kind::Member:
      return LowerMember(expression);
  }
  std::unreachable();
}

Lowerer::LoweredValue Lowerer::LowerAssignment(const frontend::Expression& expression) {
  if (expression.left->kind == frontend::Expression::Kind::Member &&
      IsDynamicExpression(*expression.left->object)) {
    return LowerDynamicAssignment(expression);
  }
  const auto& left = *expression.left;
  if (left.kind == frontend::Expression::Kind::Name) {
    return LowerNameAssignment(expression);
  }
  if (left.kind == frontend::Expression::Kind::Member) {
    return LowerMemberAssignment(expression);
  }
  if (left.kind == frontend::Expression::Kind::Index) {
    return LowerIndexAssignment(expression);
  }
  std::unreachable();
}

Lowerer::LoweredValue Lowerer::LowerName(const frontend::Expression& expression) {
  const Local* local = FindLocal(expression.text);
  if (local == nullptr && model_.globals.contains(expression.text)) {
    const auto result = AllocateRegister(expression.location);
    Emit({
        .opcode = Opcode::GetGlobal,
        .dst = result,
        .index = global_ids_.at(expression.text),
        .location = expression.location,
    });
    return {.reg = result, .type = model_.globals.at(expression.text).type};
  }
  if (local == nullptr) {
    return LowerFunctionValue(expression);
  }
  auto value = ReadLocal(*local, expression.location);
  const auto& refined = model_.expression_types.at(&expression);
  if (value.type.kind == frontend::TypeDescriptor::Kind::Dynamic &&
      refined.kind != frontend::TypeDescriptor::Kind::Dynamic) {
    return LowerConversion(value, refined, expression.location);
  }
  value.type = refined;
  return value;
}

Lowerer::LoweredValue Lowerer::LowerAwait(const frontend::Expression& expression) {
  const auto& awaited = *expression.operand;
  const auto* input = &awaited;
  if (IsFutureFactoryCall(awaited, model_) && awaited.callee->text == "value") {
    const auto& payload = *awaited.arguments.front().value;
    const auto kind = model_.expression_types.at(&payload).kind;
    using Kind = frontend::TypeDescriptor::Kind;
    if (kind == Kind::Int || kind == Kind::Double || kind == Kind::Bool || kind == Kind::Null) {
      // 直接 await 的标量工厂没有可观察的 Future 身份；保留一次载荷求值和 await 边界。
      input = &payload;
    }
  }
  const auto operand = LowerExpression(*input);
  const auto result = AllocateRegister(expression.location);
  const auto& type = model_.expression_types.at(&expression);
  const bool converts_enum = type.kind == frontend::TypeDescriptor::Kind::Enum &&
                             (operand.type.kind == frontend::TypeDescriptor::Kind::Future ||
                              operand.type.kind == frontend::TypeDescriptor::Kind::Dynamic);
  if (converts_enum) {
    RecordValueType(result, {
                                .kind = frontend::TypeDescriptor::Kind::Dynamic,
                                .name = frontend::kDynamicTypeName,
                            });
  }
  Emit({
      .opcode = Opcode::Await,
      .dst = result,
      .lhs = operand.reg,
      .location = expression.location,
  });
  if (converts_enum) {
    return LowerConversion(
        {
            .reg = result,
            .type =
                {
                    .kind = frontend::TypeDescriptor::Kind::Dynamic,
                    .name = frontend::kDynamicTypeName,
                },
        },
        type, expression.location);
  }
  return {.reg = result, .type = type};
}

Lowerer::LoweredValue Lowerer::LowerNameAssignment(const frontend::Expression& expression) {
  const auto& left = *expression.left;
  const auto& right = *expression.right;
  const Local* local = FindLocal(left.text);
  if (local == nullptr && model_.globals.contains(left.text)) {
    const auto value = LowerExpression(right);
    Emit({
        .opcode = Opcode::SetGlobal,
        .lhs = value.reg,
        .index = global_ids_.at(left.text),
        .location = left.location,
    });
    return {.reg = value.reg, .type = model_.globals.at(left.text).type};
  }
  const LoweredValue value = LowerExpression(right);
  Emit({
      .opcode = local->cell ? Opcode::SetField : Opcode::Move,
      .dst = local->reg,
      .lhs = value.reg,
      .location = left.location,
  });
  return {.reg = local->cell ? value.reg : local->reg, .type = local->type};
}

Lowerer::LoweredValue Lowerer::LowerMemberAssignment(const frontend::Expression& expression) {
  const auto& left = *expression.left;
  const auto& right = *expression.right;
  const auto member = model_.static_members.find(&left);
  if (member != model_.static_members.end()) {
    const auto value = LowerExpression(right);
    Emit({
        .opcode = Opcode::SetGlobal,
        .lhs = value.reg,
        .index = global_ids_.at(
            QualifiedMemberKey({.owner = member->second.owner, .name = member->second.member})),
        .location = left.location,
    });
    return value;
  }
  const LoweredValue object = SnapshotBefore(LowerExpression(*left.object), right);
  const auto& class_info = model_.classes.at(object.type.name);
  const auto method = class_info.methods.find(left.text);
  if (method != class_info.methods.end() && method->second.is_setter) {
    const auto value = LowerExpression(right);
    const auto& method_owner = method->second.member_owner;
    EmitSetterCall(function_ids_.at(MethodKey(method_owner, DeclarationKind::Setter, left.text)),
                   object, value.reg, left.location);
    return {.reg = value.reg, .type = method->second.setter_parameter_type.value_or(value.type)};
  }
  const auto& field = class_info.fields.at(left.text);
  const auto value = LowerExpression(right);
  Emit({
      .opcode = Opcode::SetField,
      .dst = object.reg,
      .lhs = value.reg,
      .index = field.index,
      .location = left.location,
      .field_declaration = FieldDeclaration(object.type, field.index),
  });
  return {.reg = value.reg, .type = field.type};
}

Lowerer::LoweredValue Lowerer::LowerIndexAssignment(const frontend::Expression& expression) {
  const auto& left = *expression.left;
  const auto& right = *expression.right;
  const auto array_before_index = SnapshotBefore(LowerExpression(*left.object), *left.index);
  const auto array = SnapshotBefore(array_before_index, right);
  const auto index = SnapshotBefore(LowerExpression(*left.index), right);
  const auto& element_type = model_.expression_types.at(&left);
  const auto value = LowerExpression(right);
  EmitIndexWrite(array, index.reg, value.reg, left.location);
  return {.reg = value.reg, .type = element_type};
}

std::optional<Lowerer::LoweredValue> Lowerer::LowerSuperGetter(
    const frontend::Expression& expression) {
  if (expression.object->kind == frontend::Expression::Kind::Name &&
      expression.object->text == frontend::kSuperName) {
    const auto member = model_.super_members.find(&expression);
    if (member != model_.super_members.end()) {
      const auto& signature =
          model_.classes.at(member->second.owner).methods.at(member->second.member);
      if (signature.is_getter) {
        const auto* receiver = FindLocal(frontend::kReceiverName);
        if (receiver != nullptr) {
          const LoweredValue receiver_value{.reg = receiver->reg, .type = receiver->type};
          const std::string key =
              MethodKey(member->second.owner, DeclarationKind::Getter, member->second.member);
          return LowerInvocation(expression, signature, function_ids_.at(key), &receiver_value);
        }
      }
    }
  }
  return std::nullopt;
}

Lowerer::LoweredValue Lowerer::LowerMember(const frontend::Expression& expression) {
  const auto& result_type = model_.expression_types.at(&expression);
  if (IsDynamicExpression(*expression.object)) {
    return LowerDynamicMember(expression);
  }
  if (const auto getter = LowerSuperGetter(expression)) {
    return *getter;
  }
  if (model_.interface_members.contains(&expression) ||
      model_.instance_members.contains(&expression) ||
      model_.builtin_method_values.contains(&expression) ||
      model_.super_members.contains(&expression)) {
    return LowerFunctionValue(expression);
  }
  const auto static_member = model_.static_members.find(&expression);
  if (static_member != model_.static_members.end()) {
    const auto& member = static_member->second;
    const auto& owner = model_.classes.at(member.owner);
    if (owner.methods.contains(member.member)) {
      return LowerFunctionValue(expression);
    }
    const auto result = AllocateRegister(expression.location);
    Emit({
        .opcode = Opcode::GetGlobal,
        .dst = result,
        .index = global_ids_.at(QualifiedMemberKey({.owner = member.owner, .name = member.member})),
        .location = expression.location,
    });
    return {.reg = result, .type = result_type};
  }
  if (result_type.kind == frontend::TypeDescriptor::Kind::Function) {
    const auto& object_type = model_.expression_types.at(expression.object.get());
    const auto name = frontend::NonNullableTypeName(object_type);
    const auto* owner = FindClass(name);
    if (owner != nullptr && owner->methods.contains(expression.text) &&
        !owner->methods.at(expression.text).is_getter) {
      return LowerFunctionValue(expression);
    }
  }
  if (expression.object->kind == frontend::Expression::Kind::Name &&
      FindLocal(expression.object->text) == nullptr) {
    const auto type = model_.types.find(expression.object->text);
    if (type != model_.types.end() && type->second.kind == frontend::TypeDescriptor::Kind::Enum) {
      return LowerEnumMember(expression, type->second);
    }
  }
  return LowerObjectMember(expression);
}

Lowerer::LoweredValue Lowerer::LowerObjectMember(const frontend::Expression& expression) {
  const LoweredValue object = LowerExpression(*expression.object);
  const auto object_type = model_.types.at(frontend::NonNullableTypeName(object.type));
  const auto& class_info = model_.classes.at(object_type.name);
  const auto field = class_info.fields.find(expression.text);
  if (field == class_info.fields.end()) {
    return LowerGetter(expression, object);
  }
  const RegisterId result = AllocateRegister(expression.location);
  std::optional<std::size_t> null_jump;
  if (expression.null_safe) {
    null_jump = BeginNullSafe(object.reg, expression.location);
  }
  Emit({
      .opcode = Opcode::GetField,
      .dst = result,
      .lhs = object.reg,
      .index = field->second.index,
      .location = expression.location,
      .field_declaration = FieldDeclaration(object.type, field->second.index),
  });
  FinishNullSafe(null_jump, result, expression.location);
  const auto type =
      expression.null_safe ? model_.expression_types.at(&expression) : field->second.type;
  return {.reg = result, .type = type};
}

Lowerer::LoweredValue Lowerer::LowerGetter(const frontend::Expression& expression,
                                           const LoweredValue& object) {
  const auto location = expression.location;
  const auto type = model_.types.at(object.type.name);
  const auto owner = frontend::NonNullableTypeName(type);
  const auto& method_owner = model_.classes.at(owner).methods.at(expression.text).member_owner;
  const std::string method_key = MethodKey(method_owner, DeclarationKind::Getter, expression.text);
  std::optional<std::size_t> null_jump;
  if (expression.null_safe) {
    null_jump = BeginNullSafe(object.reg, location);
  }
  const auto base = AllocateRegister(location);
  Emit({.opcode = Opcode::Move, .dst = base, .lhs = object.reg, .location = location});
  const auto result = AllocateRegister(location);
  const auto& result_type = model_.expression_types.at(&expression);
  RecordValueType(result, result_type);
  Emit({
      .opcode = Opcode::Call,
      .dst = result,
      .lhs = base,
      .index = function_ids_.at(method_key),
      .location = location,
      .binding_receiver = ReceiverDeclaration(object.type),
  });
  FinishNullSafe(null_jump, result, location);
  return {.reg = result, .type = result_type};
}

Lowerer::LoweredValue Lowerer::LowerCall(const frontend::Expression& expression) {
  if (expression.callee->kind == frontend::Expression::Kind::Member &&
      IsDynamicExpression(*expression.callee->object)) {
    return LowerDynamicMethodCall(expression);
  }
  const auto callee_type = model_.expression_types.find(expression.callee.get());
  if (callee_type != model_.expression_types.end() &&
      callee_type->second.kind == frontend::TypeDescriptor::Kind::Dynamic) {
    return LowerDynamicCall(expression);
  }
  if (model_.interface_members.contains(&expression)) {
    return LowerInterfaceCall(expression);
  }
  const auto static_member = model_.static_members.find(&expression);
  if (static_member != model_.static_members.end()) {
    const auto& member = static_member->second;
    const auto& signature = model_.classes.at(member.owner).methods.at(member.member);
    return LowerInvocation(
        expression, signature,
        function_ids_.at(QualifiedMemberKey({.owner = member.owner, .name = member.member})),
        nullptr);
  }
  if (model_.instance_members.contains(&expression) ||
      model_.super_members.contains(expression.callee.get())) {
    return LowerMethodCall(expression);
  }
  if (callee_type != model_.expression_types.end() &&
      callee_type->second.kind == frontend::TypeDescriptor::Kind::Function) {
    return LowerIndirectCall(expression);
  }
  if (expression.callee->kind == frontend::Expression::Kind::Name &&
      expression.callee->text == frontend::kSleepFunctionName) {
    const auto delay = LowerExpression(*expression.arguments.front().value);
    const auto result = AllocateRegister(expression.location);
    Emit({
        .opcode = Opcode::SleepFuture,
        .dst = result,
        .lhs = delay.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }
  if (expression.callee->kind == frontend::Expression::Kind::Name &&
      expression.callee->text == frontend::kSuperName) {
    const auto& owner = model_.classes.at(current_owner_->name);
    const auto& parent = owner.base_class;
    if (!parent) {
      std::unreachable();
    }
    const auto& base = model_.classes.at(*parent);
    const auto* receiver = FindLocal(frontend::kReceiverName);
    return LowerConstructorCall(expression.arguments, model_.argument_order.at(&expression),
                                expression.location, base, receiver->reg);
  }
  if (IsFutureFactoryCall(expression, model_)) {
    return LowerFutureCall(expression);
  }
  if (expression.callee->kind == frontend::Expression::Kind::Name) {
    return LowerNamedCall(expression);
  }
  // 其余合法调用已由 checker 解析为实例方法；这里不再以 InvalidType 探测多个入口。
  return LowerMethodCall(expression);
}

Lowerer::LoweredValue Lowerer::LowerNamedCall(const frontend::Expression& expression) {
  const auto& callee = *expression.callee;
  if (callee.text == "print") {
    const auto value = LowerExpression(*expression.arguments.front().value);
    Emit({.opcode = Opcode::Print, .lhs = value.reg, .location = expression.location});
    const auto result = AllocateRegister(expression.location);
    Emit({.opcode = Opcode::LoadNull, .dst = result, .location = expression.location});
    return {
        .reg = result,
        .type = {.kind = frontend::TypeDescriptor::Kind::Void, .name = frontend::kVoidTypeName},
    };
  }
  const auto application = model_.generic_applications.find(&callee);
  const auto& name =
      application == model_.generic_applications.end() ? callee.text : application->second;
  const auto class_info = model_.classes.find(name);
  if (class_info != model_.classes.end()) {
    return LowerConstruction(expression, class_info->second.type);
  }
  const auto alias = model_.types.find(name);
  if (alias != model_.types.end() && alias->second.kind == frontend::TypeDescriptor::Kind::Class) {
    return LowerConstruction(expression, alias->second);
  }
  return LowerInvocation(expression, model_.functions.at(name), function_ids_.at(name), nullptr);
}

Lowerer::LoweredValue Lowerer::LowerMethodCall(const frontend::Expression& expression) {
  const auto& callee = *expression.callee;
  const auto super = model_.super_members.find(&callee);
  const auto& target =
      super == model_.super_members.end() ? model_.instance_members.at(&expression) : super->second;
  const auto receiver = super == model_.super_members.end()
                            ? LowerExpression(*callee.object)
                            : ReadLocal(*FindLocal(frontend::kReceiverName), callee.location);
  const auto& method = model_.classes.at(target.owner).methods.at(target.member);
  const auto kind = MethodDeclarationKind(method);
  const auto method_key = MethodKey(target.owner, kind, target.member);
  const auto method_id = function_ids_.at(method_key);
  if (!callee.null_safe) {
    return LowerInvocation(expression, method, method_id, &receiver);
  }
  const auto null_jump = BeginNullSafe(receiver.reg, expression.location);
  auto result = LowerInvocation(expression, method, method_id, &receiver);
  FinishNullSafe(null_jump, result.reg, expression.location);
  result.type = model_.expression_types.at(&expression);
  return result;
}

Lowerer::LoweredValue Lowerer::LowerFutureCall(const frontend::Expression& expression) {
  const auto& result_type = model_.expression_types.at(&expression);
  const auto& method = expression.callee->text;
  auto payload = LowerExpression(*expression.arguments.front().value);
  if (method == "value") {
    payload.reg = LowerFuturePayload(payload, result_type, expression.location);
  }
  const auto result = AllocateRegister(expression.location);
  if (method == "sync") {
    const auto base = next_register_;
    const auto callable = AllocateRegister(expression.location);
    Emit({
        .opcode = Opcode::Move,
        .dst = callable,
        .lhs = payload.reg,
        .location = expression.location,
    });
    Emit({
        .opcode = Opcode::Call,
        .dst = result,
        .lhs = base,
        .immediate = 1,
        .index = AddSyncFutureFunction(result_type, payload.type),
        .location = expression.location,
    });
  } else {
    Emit({
        .opcode = Opcode::NewFuture,
        .dst = result,
        .lhs = payload.reg,
        .rhs = method == "error" ? 1U : 0U,
        .location = expression.location,
    });
  }
  return {.reg = result, .type = result_type};
}

std::uint32_t Lowerer::AddSyncFutureFunction(const frontend::TypeDescriptor& return_type,
                                             const frontend::TypeDescriptor& callback_type) {
  Function function;
  function.name = "$future.sync." + std::to_string(program_.functions.size());
  const auto index = AddFunctionId(function.name);
  function.return_type = return_type;
  function.parameter_count = 1;
  function.is_async = true;
  function.signature.emplace();
  function.signature->parameters.push_back(
      {.name = "callback", .type = RegisterRuntimeType(callback_type)});
  function.signature->result = RegisterRuntimeType(return_type);
  function.value_types = {callback_type};
  auto* previous_function = current_function_;
  const auto previous_register = next_register_;
  const auto previous_handler = current_handler_;
  BeginGeneratedBody(function);
  const auto& payload = model_.generic_arguments.at(return_type.name).front();
  const auto result = AllocateRegister({});
  RecordValueType(result, payload);
  Emit({
      .opcode = Opcode::CallIndirect,
      .dst = result,
      .lhs = next_register_,
      .rhs = 0,
      .location = {},
  });
  LoweredValue value{.reg = result, .type = payload};
  if (payload.kind == frontend::TypeDescriptor::Kind::Enum) {
    value = LowerConversion(
        value,
        {.kind = frontend::TypeDescriptor::Kind::Dynamic, .name = frontend::kDynamicTypeName}, {});
    RecordValueType(value.reg, value.type, ValueTypeRecording::Declare);
  }
  Emit({.opcode = Opcode::Return, .dst = value.reg, .location = {}});
  function.register_count = next_register_;
  current_function_ = previous_function;
  current_handler_ = previous_handler;
  next_register_ = previous_register;
  StoreFunction(std::move(function));
  return index;
}

Lowerer::LoweredValue Lowerer::LowerInvocation(const frontend::Expression& expression,
                                               const frontend::FunctionDescriptor& function,
                                               std::uint32_t function_id,
                                               const LoweredValue* receiver,
                                               Opcode opcode) {
  const std::size_t receiver_count = receiver == nullptr ? 0 : 1;
  const std::size_t count = receiver_count + function.parameters.size();
  const auto base = AllocateRegisterWindow(count, expression.location);
  if (receiver != nullptr) {
    Emit({
        .opcode = Opcode::Move,
        .dst = base,
        .lhs = receiver->reg,
        .location = expression.location,
    });
  }
  if (!function.optional_parameters.empty()) {
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      if (function.optional_parameters.contains(function.parameters.at(index).name)) {
        Emit({
            .opcode = function.intrinsic == Builtin::DateFromUTC ||
                              (function.intrinsic == Builtin::ByteDataView && index == 1)
                          ? Opcode::LoadInt
                          : Opcode::LoadNull,
            .dst = static_cast<RegisterId>(base + receiver_count + index),
            .location = expression.location,
        });
      }
    }
  }
  LowerArguments(expression, static_cast<RegisterId>(base + receiver_count));
  std::optional<DeclarationId> binding_receiver;
  if (receiver != nullptr && opcode == Opcode::Call) {
    binding_receiver = ReceiverDeclaration(receiver->type);
    const auto* member =
        expression.kind == frontend::Expression::Kind::Call ? expression.callee.get() : &expression;
    const auto super = model_.super_members.find(member);
    if (super != model_.super_members.end()) {
      const auto& self = FindLocal(frontend::kReceiverName)->type;
      const auto& parent = model_.classes.at(self.name).base_class;
      if (!parent) {
        std::unreachable();
      }
      binding_receiver = declarations_->classes.at(*parent).declaration;
    }
  }
  const RegisterId result = AllocateRegister(expression.location);
  RecordValueType(result, function.return_type);
  Emit({
      .opcode = opcode,
      .dst = result,
      .lhs = base,
      .immediate = static_cast<std::int64_t>(count),
      .index = function_id,
      .location = expression.location,
      .binding_receiver = binding_receiver,
  });
  return {.reg = result, .type = function.return_type};
}

void Lowerer::LowerArguments(const frontend::Expression& expression, RegisterId base) {
  if (expression.arguments.empty()) {
    return;
  }
  const auto& order = model_.argument_order.at(&expression);
  for (std::size_t source_index = 0; source_index < expression.arguments.size(); ++source_index) {
    const auto& argument = expression.arguments.at(source_index);
    const auto index = order.at(source_index);
    const auto value = LowerExpression(*argument.value);
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + index),
        .lhs = value.reg,
        .location = argument.location,
    });
  }
}

const frontend::ClassDescriptor* Lowerer::FindClass(std::string_view name) const {
  const auto found = model_.classes.find(name);
  return found == model_.classes.end() ? nullptr : &found->second;
}

const Lowerer::Local* Lowerer::FindLocal(std::string_view name) const {
  for (const auto& scope : std::views::reverse(scopes_)) {
    const auto found = scope.find(name);
    if (found != scope.end()) {
      return &found->second;
    }
  }
  return nullptr;
}

RegisterId Lowerer::AllocateRegister(frontend::SourceLocation location) {
  if (next_register_ == std::numeric_limits<RegisterId>::max()) {
    AddDiagnostic(location, "function uses too many registers");
    return next_register_;
  }
  return next_register_++;
}

RegisterId Lowerer::AllocateRegisterWindow(std::size_t count, frontend::SourceLocation location) {
  const auto base = next_register_;
  for (std::size_t index = 0; index < count; ++index) {
    static_cast<void>(AllocateRegister(location));
  }
  return base;
}

bool Lowerer::IsInteger(const frontend::TypeDescriptor& type) {
  return type.kind == frontend::TypeDescriptor::Kind::Int ||
         type.kind == frontend::TypeDescriptor::Kind::Enum;
}

bool Lowerer::IsDynamicExpression(const frontend::Expression& expression) const {
  const auto type = model_.expression_types.find(&expression);
  return type != model_.expression_types.end() &&
         type->second.kind == frontend::TypeDescriptor::Kind::Dynamic;
}

void Lowerer::BindJump(std::size_t jump) {
  current_function_->instructions.at(jump).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
}

void Lowerer::Emit(Instruction instruction) {
  if (instruction.opcode == Opcode::GetIndex || instruction.opcode == Opcode::SetIndex ||
      (instruction.opcode == Opcode::BuiltinCall &&
       instruction.index == static_cast<std::uint32_t>(Builtin::ArrayPush))) {
    const auto array = instruction.opcode == Opcode::SetIndex ? instruction.dst : instruction.lhs;
    const auto type = current_function_->value_types.at(array);
    instruction.array_type = RegisterArrayType(type);
  }
  RecordInstructionValueType(instruction);
  instruction.handler = current_handler_;
  current_function_->instructions.push_back(std::move(instruction));
}

std::optional<DeclarationId> Lowerer::ReceiverDeclaration(
    const frontend::TypeDescriptor& type) const {
  const auto owner = declarations_->classes.find(frontend::NonNullableTypeName(type));
  return owner == declarations_->classes.end() ? std::nullopt
                                               : std::optional(owner->second.declaration);
}

std::optional<DeclarationId> Lowerer::FieldDeclaration(const frontend::TypeDescriptor& type,
                                                       FieldId index) const {
  if (native_mode_ != NativeProgramMode::Reloadable) {
    return std::nullopt;
  }
  const auto name = frontend::NonNullableTypeName(type);
  const auto& owner = program_.classes[class_ids_.at(name)];
  if (owner.kind != Class::Kind::Instance || owner.callable) {
    return std::nullopt;
  }
  return owner.fields[index].declaration;
}

std::size_t Lowerer::BeginNullSafe(RegisterId receiver, frontend::SourceLocation location) {
  const auto jump = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfNull, .lhs = receiver, .location = location});
  return jump;
}

void Lowerer::FinishNullSafe(std::optional<std::size_t> null_jump,
                             RegisterId result,
                             frontend::SourceLocation location) {
  if (!null_jump) {
    return;
  }
  const auto end_jump = current_function_->instructions.size();
  Emit({.opcode = Opcode::Jump, .location = location});
  BindJump(*null_jump);
  Emit({.opcode = Opcode::LoadNull, .dst = result, .location = location});
  BindJump(end_jump);
}

void Lowerer::PushScope() {
  scopes_.emplace_back();
}

void Lowerer::PopScope() {
  scopes_.pop_back();
}

void Lowerer::DeclareLocal(std::string_view name,
                           RegisterId reg,
                           frontend::TypeDescriptor type,
                           frontend::SourceLocation location,
                           const void* declaration) {
  RecordValueType(reg, type, ValueTypeRecording::Declare);
  const auto binding = model_.bindings.find(declaration);
  const auto binding_id = binding == model_.bindings.end() ? 0 : binding->second;
  const bool cell =
      binding != model_.bindings.end() && model_.captured_mutables.contains(binding_id);
  if (cell) {
    const auto boxed = AllocateRegister(location);
    Emit({
        .opcode = Opcode::NewInstance,
        .dst = boxed,
        .index = class_ids_.at("$Cell"),
        .location = location,
    });
    Emit({.opcode = Opcode::SetField, .dst = boxed, .lhs = reg, .location = location});
    reg = boxed;
  }
  // 同作用域的名称唯一性由 checker 保证；这里只建立寄存器与绑定的对应关系。
  scopes_.back().emplace(name, Local{.reg = reg, .type = type, .cell = cell});
  if (binding != model_.bindings.end()) {
    binding_registers_.insert_or_assign(binding_id, reg);
  }
}

void Lowerer::LowerVariableDeclaration(const frontend::Statement& statement) {
  const RegisterId local = AllocateRegister(statement.location);
  const auto& type = model_.local_types.at(&statement);
  const auto initializer = LowerExpression(*statement.expression);
  Emit({
      .opcode = Opcode::Move,
      .dst = local,
      .lhs = initializer.reg,
      .location = statement.location,
  });
  DeclareLocal(statement.name, local, type, statement.location, &statement);
}

Lowerer::LoweredValue Lowerer::LowerIntegerLiteral(const frontend::Expression& expression) {
  const auto value = expression.IntegerValue();
  // checker 已验证有符号范围；无效字面量不会进入 lowering。
  if (!value) {
    std::unreachable();
  }
  const RegisterId reg = AllocateRegister(expression.location);
  Emit({
      .opcode = Opcode::LoadInt,
      .dst = reg,
      .immediate = *value,
      .location = expression.location,
  });
  return {
      .reg = reg,
      .type = {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName},
  };
}

Lowerer::LoweredValue Lowerer::EmitStringConstant(std::string_view value,
                                                  frontend::SourceLocation location) {
  const auto [found, inserted] =
      string_constant_ids_.try_emplace(value, static_cast<std::uint32_t>(program_.strings.size()));
  const auto index = found->second;
  if (inserted) {
    program_.strings.emplace_back(value);
  }
  const RegisterId reg = AllocateRegister(location);
  Emit({.opcode = Opcode::LoadString, .dst = reg, .index = index, .location = location});
  return {
      .reg = reg,
      .type = {.kind = frontend::TypeDescriptor::Kind::String, .name = frontend::kStringTypeName},
  };
}

Lowerer::LoweredValue Lowerer::LowerEnumMember(const frontend::Expression& expression,
                                               const frontend::TypeDescriptor& type) {
  const auto ordinal =
      model_.enum_values.at(QualifiedMemberKey({.owner = type.name, .name = expression.text}));
  const RegisterId reg = AllocateRegister(expression.location);
  Emit({
      .opcode = Opcode::LoadInt,
      .dst = reg,
      .immediate = ordinal,
      .location = expression.location,
  });
  return {.reg = reg, .type = type};
}

Lowerer::LoweredValue Lowerer::LowerIncrement(const frontend::Expression& expression) {
  const auto& target = *expression.operand;
  const auto destination = LowerMutationTarget(target);
  auto operand = destination.value;
  if (operand.type.kind == frontend::TypeDescriptor::Kind::Dynamic) {
    operand = LowerConversion(
        operand, {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName},
        expression.location);
  }
  const auto one = EmitTemporary({
      .opcode = Opcode::LoadInt,
      .immediate = 1,
      .location = expression.location,
  });
  const auto result = AllocateRegister(expression.location);
  Emit({
      .opcode = expression.operation == TokenKind::PlusPlus ? Opcode::AddInt : Opcode::SubInt,
      .dst = result,
      .lhs = operand.reg,
      .rhs = one,
      .location = expression.location,
  });
  RegisterId returned = expression.postfix ? AllocateRegister(expression.location) : result;
  if (expression.postfix) {
    Emit({
        .opcode = Opcode::Move,
        .dst = returned,
        .lhs = operand.reg,
        .location = expression.location,
    });
  }
  StoreMutationTarget(destination, result, expression.location);
  return {.reg = returned, .type = operand.type};
}

Lowerer::MutationTarget Lowerer::LowerMutationTarget(const frontend::Expression& target) {
  MutationTarget destination;
  if (target.kind == frontend::Expression::Kind::Name) {
    if (const auto* local = FindLocal(target.text)) {
      destination.kind = local->cell ? MutationTarget::Kind::Field : MutationTarget::Kind::Local;
      destination.receiver = {
          .reg = local->reg,
          .type = local->cell
                      ? frontend::TypeDescriptor{.kind = frontend::TypeDescriptor::Kind::Class,
                                                 .name = "$Cell",}
                      : local->type,
      };
      destination.value = ReadLocal(*local, target.location);
    } else {
      destination.kind = MutationTarget::Kind::Global;
      destination.slot = global_ids_.at(target.text);
      destination.value = LowerExpression(target);
    }
    return destination;
  }
  const auto static_member = model_.static_members.find(&target);
  if (static_member != model_.static_members.end()) {
    const auto& member = static_member->second;
    destination.kind = MutationTarget::Kind::Global;
    destination.slot =
        global_ids_.at(QualifiedMemberKey({.owner = member.owner, .name = member.member}));
    destination.value = LowerMember(target);
    RecordValueType(destination.value.reg, destination.value.type);
    return destination;
  }
  if (target.kind == frontend::Expression::Kind::Member) {
    auto receiver = LowerExpression(*target.object);
    receiver.reg =
        EmitTemporary({.opcode = Opcode::Move, .lhs = receiver.reg, .location = target.location});
    if (receiver.type.kind == frontend::TypeDescriptor::Kind::Dynamic) {
      const auto text = EmitStringConstant(target.text, target.location);
      const auto value = AllocateRegister(target.location);
      const std::array values{receiver.reg, text.reg};
      EmitBuiltinCall(Builtin::DynamicMemberGet, value, values, target.location);
      destination.kind = MutationTarget::Kind::Member;
      destination.receiver = receiver;
      destination.index = text.reg;
      destination.value = {.reg = value, .type = model_.expression_types.at(&target)};
      return destination;
    }
    const auto& owner = model_.classes.at(receiver.type.name);
    const auto field = owner.fields.find(target.text);
    destination.receiver = receiver;
    if (field == owner.fields.end()) {
      const auto& method = owner.methods.at(target.text);
      destination.kind = MutationTarget::Kind::Setter;
      destination.slot =
          function_ids_.at(MethodKey(method.member_owner, DeclarationKind::Setter, target.text));
      destination.value = LowerGetter(target, receiver);
      return destination;
    }
    destination.kind = MutationTarget::Kind::Field;
    destination.slot = field->second.index;
    destination.value = {
        .reg = EmitTemporary({
            .opcode = Opcode::GetField,
            .lhs = receiver.reg,
            .index = destination.slot,
            .location = target.location,
            .field_declaration = FieldDeclaration(receiver.type, destination.slot),
        }),
        .type = model_.expression_types.at(&target),
    };
    RecordValueType(destination.value.reg, destination.value.type);
    return destination;
  }
  if (target.kind == frontend::Expression::Kind::Index) {
    auto receiver = SnapshotBefore(LowerExpression(*target.object), *target.index);
    receiver.reg =
        EmitTemporary({.opcode = Opcode::Move, .lhs = receiver.reg, .location = target.location});
    auto position = LowerExpression(*target.index);
    position.reg =
        EmitTemporary({.opcode = Opcode::Move, .lhs = position.reg, .location = target.location});
    destination.kind = MutationTarget::Kind::Index;
    destination.receiver = receiver;
    destination.index = position.reg;
    const auto value = AllocateRegister(target.location);
    EmitIndexRead(value, receiver, position.reg, target.location);
    destination.value = {.reg = value, .type = model_.expression_types.at(&target)};
    RecordValueType(value, destination.value.type);
    return destination;
  }
  std::unreachable();
}

void Lowerer::EmitSetterCall(std::uint32_t target,
                             const LoweredValue& receiver,
                             RegisterId value,
                             frontend::SourceLocation location) {
  const auto base = AllocateRegisterWindow(2, location);
  Emit({.opcode = Opcode::Move, .dst = base, .lhs = receiver.reg, .location = location});
  Emit({.opcode = Opcode::Move, .dst = base + 1, .lhs = value, .location = location});
  const auto ignored = AllocateRegister(location);
  Emit({
      .opcode = Opcode::Call,
      .dst = ignored,
      .lhs = base,
      .index = target,
      .location = location,
      .binding_receiver = ReceiverDeclaration(receiver.type),
  });
}

void Lowerer::StoreMutationTarget(const MutationTarget& destination,
                                  RegisterId value,
                                  frontend::SourceLocation location) {
  const auto& receiver = destination.receiver;
  switch (destination.kind) {
    case MutationTarget::Kind::Global:
      Emit({
          .opcode = Opcode::SetGlobal,
          .lhs = value,
          .index = destination.slot,
          .location = location,
      });
      return;
    case MutationTarget::Kind::Setter:
      EmitSetterCall(destination.slot, receiver, value, location);
      return;
    case MutationTarget::Kind::Local:
      Emit({
          .opcode = Opcode::Move,
          .dst = receiver.reg,
          .lhs = value,
          .location = location,
      });
      return;
    case MutationTarget::Kind::Field:
      Emit({
          .opcode = Opcode::SetField,
          .dst = receiver.reg,
          .lhs = value,
          .index = destination.slot,
          .location = location,
          .field_declaration = FieldDeclaration(receiver.type, destination.slot),
      });
      return;
    case MutationTarget::Kind::Member: {
      const std::array values{receiver.reg, destination.index, value};
      const auto result = AllocateRegister(location);
      EmitBuiltinCall(Builtin::DynamicMemberSet, result, values, location);
      return;
    }
    case MutationTarget::Kind::Index:
      EmitIndexWrite(receiver, destination.index, value, location);
      return;
  }
}

Lowerer::LoweredValue Lowerer::LowerUnary(const frontend::Expression& expression) {
  if (expression.operation == TokenKind::PlusPlus ||
      expression.operation == TokenKind::MinusMinus) {
    return LowerIncrement(expression);
  }
  const LoweredValue operand = LowerExpression(*expression.operand);
  const RegisterId result = AllocateRegister(expression.location);
  if (expression.operation == TokenKind::Minus &&
      operand.type.kind == frontend::TypeDescriptor::Kind::Dynamic) {
    const std::array values{operand.reg};
    EmitBuiltinCall(Builtin::DynamicNegate, result, values, expression.location);
    return {.reg = result, .type = operand.type};
  }
  if (expression.operation == TokenKind::Minus &&
      operand.type.kind == frontend::TypeDescriptor::Kind::Double) {
    Emit({
        .opcode = Opcode::NegDouble,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  if (expression.operation == TokenKind::Minus && IsInteger(operand.type)) {
    Emit({
        .opcode = Opcode::NegInt,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  if (expression.operation == TokenKind::Bang &&
      operand.type.kind == frontend::TypeDescriptor::Kind::Bool) {
    Emit({
        .opcode = Opcode::NotBool,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  if (expression.operation == TokenKind::Tilde && IsInteger(operand.type)) {
    Emit({
        .opcode = Opcode::BitNotInt,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  std::unreachable();
}

bool Lowerer::HasAssignments(const frontend::Expression& expression) {
  if (const auto found = assignment_effects_.find(&expression);
      found != assignment_effects_.end()) {
    return found->second;
  }
  const auto result = [this, &expression] {
    if (expression.kind == frontend::Expression::Kind::Call) {
      return true;
    }
    if (expression.kind == frontend::Expression::Kind::CompoundAssignment ||
        (expression.kind == frontend::Expression::Kind::Binary &&
         expression.operation == TokenKind::Equal)) {
      return true;
    }
    if (expression.kind == frontend::Expression::Kind::Unary &&
        (expression.operation == TokenKind::PlusPlus ||
         expression.operation == TokenKind::MinusMinus)) {
      return true;
    }
    for (const auto* child : ExpressionOperands(expression)) {
      if (child != nullptr && HasAssignments(*child)) {
        return true;
      }
    }
    return std::ranges::any_of(
               expression.elements,
               [this](const auto& child) -> bool { return HasAssignments(*child); }) ||
           std::ranges::any_of(expression.entries,
                               [this](const auto& entry) -> bool {
                                 return HasAssignments(*entry.key) || HasAssignments(*entry.value);
                               }) ||
           std::ranges::any_of(
               expression.arms,
               [this](const auto& arm) -> bool { return HasAssignments(*arm.value); }) ||
           std::ranges::any_of(expression.arguments, [this](const auto& argument) -> bool {
             return HasAssignments(*argument.value);
           });
  }();
  assignment_effects_.emplace(&expression, result);
  return result;
}

Lowerer::LoweredValue Lowerer::SnapshotBefore(LoweredValue value,
                                              const frontend::Expression& later) {
  if (HasAssignments(later)) {
    const auto snapshot = AllocateRegister(later.location);
    Emit({.opcode = Opcode::Move, .dst = snapshot, .lhs = value.reg, .location = later.location});
    value.reg = snapshot;
  }
  return value;
}

Lowerer::LoweredValue Lowerer::LowerBinary(const frontend::Expression& expression) {
  if (expression.operation == TokenKind::Equal) {
    return LowerAssignment(expression);
  }
  if (expression.operation == TokenKind::QuestionQuestion) {
    const auto left = LowerExpression(*expression.left);
    return LowerBranchValue(
        expression, Opcode::JumpIfNull, left.reg, [&] { return left; },
        [&] { return LowerExpression(*expression.right); });
  }
  if (expression.operation == TokenKind::AndAnd || expression.operation == TokenKind::OrOr) {
    return LowerLogical(expression);
  }
  if (expression.operation == TokenKind::Plus &&
      model_.expression_types.at(&expression).kind == frontend::TypeDescriptor::Kind::Int &&
      expression.left != nullptr && expression.left->kind == frontend::Expression::Kind::Binary &&
      expression.left->operation == TokenKind::Plus) {
    return LowerIntegerAdditionChain(expression);
  }
  return LowerBinaryValue(expression);
}

Lowerer::LoweredValue Lowerer::LowerIntegerAdditionChain(const frontend::Expression& expression) {
  std::vector<const frontend::Expression*> rights;
  const frontend::Expression* leftmost = &expression;
  while (leftmost->left != nullptr && leftmost->left->kind == frontend::Expression::Kind::Binary &&
         leftmost->left->operation == TokenKind::Plus) {
    rights.push_back(leftmost->right.get());
    leftmost = leftmost->left.get();
  }
  rights.push_back(leftmost->right.get());
  auto value = LowerExpression(*leftmost->left);
  for (const auto* right_expression : std::views::reverse(rights)) {
    const auto right = LowerExpression(*right_expression);
    const auto result = AllocateRegister(expression.location);
    Emit({
        .opcode = Opcode::AddInt,
        .dst = result,
        .lhs = value.reg,
        .rhs = right.reg,
        .location = expression.location,
    });
    value = {.reg = result, .type = value.type};
  }
  return value;
}

Lowerer::LoweredValue Lowerer::LowerCompoundAssignment(const frontend::Expression& expression) {
  const auto operation = expression.operation;
  const auto& left = *expression.left;
  if (model_.expression_types.at(&left).kind == frontend::TypeDescriptor::Kind::Dynamic) {
    const auto opcode = SelectIntegerOpcode(operation);
    return LowerDynamicCompoundAssignment(expression, opcode);
  }
  auto target = LowerMutationTarget(left);
  target.value = SnapshotBefore(target.value, *expression.right);
  const auto right = LowerExpression(*expression.right);
  const auto opcode = SelectBinaryOpcode(operation, target.value.type);
  const auto result = EmitTemporary({
      .opcode = opcode,
      .lhs = target.value.reg,
      .rhs = right.reg,
      .location = expression.location,
  });
  StoreMutationTarget(target, result, expression.location);
  return {.reg = result, .type = target.value.type};
}

Lowerer::LoweredValue Lowerer::LowerBinaryValue(const frontend::Expression& expression) {
  const auto& result_type = model_.expression_types.at(&expression);
  const auto left = SnapshotBefore(LowerExpression(*expression.left), *expression.right);
  const LoweredValue right = LowerExpression(*expression.right);
  using Kind = frontend::TypeDescriptor::Kind;
  if (left.type.kind == Kind::Dynamic || right.type.kind == Kind::Dynamic) {
    const auto operation = SelectIntegerOpcode(expression.operation);
    const auto selector = EmitTemporary({
        .opcode = Opcode::LoadInt,
        .immediate = static_cast<std::int64_t>(operation),
        .location = expression.location,
    });
    const auto result = AllocateRegister(expression.location);
    const std::array values{selector, left.reg, right.reg};
    EmitBuiltinCall(Builtin::DynamicBinary, result, values, expression.location);
    return {.reg = result, .type = result_type};
  }
  if (expression.operation == TokenKind::Plus && result_type.kind == Kind::String) {
    const auto result = AllocateRegister(expression.location);
    Emit({
        .opcode = Opcode::StringConcat,
        .dst = result,
        .lhs = left.reg,
        .rhs = right.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = {.kind = Kind::String, .name = frontend::kStringTypeName}};
  }
  if ((expression.operation == TokenKind::EqualEqual ||
       expression.operation == TokenKind::BangEqual) &&
      (left.type.nullable || right.type.nullable || left.type.kind == Kind::Null ||
       right.type.kind == Kind::Null || left.type.kind == Kind::Class ||
       left.type.kind == Kind::Interface || left.type.kind == Kind::Function ||
       left.type.kind == Kind::ByteArray || left.type.kind == Kind::ByteData ||
       left.type.kind == Kind::RegExp || left.type.kind == Kind::RegExpMatch)) {
    const auto result = AllocateRegister(expression.location);
    Emit({
        .opcode = expression.operation == TokenKind::EqualEqual ? Opcode::EqualValue
                                                                : Opcode::NotEqualValue,
        .dst = result,
        .lhs = left.reg,
        .rhs = right.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = {.kind = Kind::Bool, .name = frontend::kBoolTypeName}};
  }
  const auto opcode = SelectBinaryOpcode(expression.operation, left.type);
  const auto result = EmitTemporary({
      .opcode = opcode,
      .lhs = left.reg,
      .rhs = right.reg,
      .location = expression.location,
  });
  return {.reg = result, .type = result_type};
}

Lowerer::LoweredValue Lowerer::LowerConditional(const frontend::Expression& expression) {
  const auto condition = LowerExpression(*expression.operand);
  return LowerBranchValue(
      expression, Opcode::JumpIfFalse, condition.reg,
      [&] { return LowerExpression(*expression.left); },
      [&] { return LowerExpression(*expression.right); });
}

Lowerer::LoweredValue Lowerer::LowerLogical(const frontend::Expression& expression) {
  const LoweredValue left = LowerExpression(*expression.left);
  const auto right = [&] { return LowerExpression(*expression.right); };
  const auto constant = [&]() -> LoweredValue {
    const auto reg = AllocateRegister(expression.location);
    Emit({
        .opcode = Opcode::LoadBool,
        .dst = reg,
        .immediate = expression.operation == TokenKind::OrOr ? 1 : 0,
        .location = expression.location,
    });
    return {
        .reg = reg,
        .type = {.kind = frontend::TypeDescriptor::Kind::Bool, .name = frontend::kBoolTypeName},
    };
  };
  if (expression.operation == TokenKind::AndAnd) {
    return LowerBranchValue(expression, Opcode::JumpIfFalse, left.reg, right, constant);
  }
  return LowerBranchValue(expression, Opcode::JumpIfFalse, left.reg, constant, right);
}

}  // namespace vela::ir
