#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <array>
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
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

frontend::TypeDescriptor InvalidType() {
  return {};
}

std::string MethodKey(std::string_view owner, const frontend::Function& method) {
  std::string key(owner);
  key += '.';
  if (method.is_getter) {
    key += "$get.";
  } else if (method.is_setter) {
    key += "$set.";
  }
  key += method.name;
  return key;
}

bool HasAssignments(const frontend::Expression& expression) {
  if (expression.kind == frontend::Expression::Kind::Call) {
    return true;
  }
  if (expression.kind == frontend::Expression::Kind::Binary &&
      (expression.text == "=" || frontend::CompoundAssignmentOperation(expression.text))) {
    return true;
  }
  if (expression.kind == frontend::Expression::Kind::Unary &&
      (expression.text == "++" || expression.text == "--")) {
    return true;
  }
  for (const auto* child : ExpressionOperands(expression)) {
    if (child != nullptr && HasAssignments(*child)) {
      return true;
    }
  }
  return std::ranges::any_of(expression.elements,
                             [](const auto& child) -> bool { return HasAssignments(*child); }) ||
         std::ranges::any_of(expression.entries,
                             [](const auto& entry) -> bool {
                               return HasAssignments(*entry.key) || HasAssignments(*entry.value);
                             }) ||
         std::ranges::any_of(expression.arms,
                             [](const auto& arm) -> bool { return HasAssignments(*arm.value); }) ||
         std::ranges::any_of(expression.arguments, [](const auto& argument) -> bool {
           return HasAssignments(*argument.value);
         });
}

std::optional<Opcode> SelectIntegerOpcode(std::string_view operation) {
  constexpr std::array<std::pair<std::string_view, Opcode>, 17> integer_operations = {
      {
          {"+", Opcode::AddInt},
          {"-", Opcode::SubInt},
          {"*", Opcode::MulInt},
          {"/", Opcode::DivInt},
          {"%", Opcode::ModInt},
          {"&", Opcode::BitAndInt},
          {"|", Opcode::BitOrInt},
          {"^", Opcode::BitXorInt},
          {"<<", Opcode::ShiftLeftInt},
          {">>", Opcode::ShiftRightInt},
          {">>>", Opcode::ShiftUnsignedRightInt},
          {"==", Opcode::EqualInt},
          {"!=", Opcode::NotEqualInt},
          {"<", Opcode::LessInt},
          {"<=", Opcode::LessEqualInt},
          {">", Opcode::GreaterInt},
          {">=", Opcode::GreaterEqualInt},
      },
  };
  const auto found = std::ranges::find_if(
      integer_operations, [&](const auto& entry) { return entry.first == operation; });
  if (found != integer_operations.end()) {
    return found->second;
  }
  return std::nullopt;
}

}  // namespace

std::optional<Opcode> Lowerer::SelectBinaryOpcode(std::string_view operation,
                                                  const frontend::TypeDescriptor& type) {
  using Kind = frontend::TypeDescriptor::Kind;
  if (type.kind == Kind::Double) {
    return SelectDoubleOpcode(operation);
  }
  if (type.kind == Kind::String) {
    if (operation == "+") {
      return Opcode::StringConcat;
    }
    if (operation == "==") {
      return Opcode::EqualString;
    }
    if (operation == "!=") {
      return Opcode::NotEqualString;
    }
    return std::nullopt;
  }
  if (type.kind == Kind::Bool) {
    if (operation == "==") {
      return Opcode::EqualBool;
    }
    if (operation == "!=") {
      return Opcode::NotEqualBool;
    }
    return std::nullopt;
  }
  return SelectIntegerOpcode(operation);
}

LoweringResult Lowerer::Lower() {
  program_ = {};
  program_.native_mode = native_mode_;
  diagnostics_.clear();
  class_ids_.clear();
  function_ids_.clear();
  array_type_ids_.clear();
  callable_classes_.clear();
  global_ids_.clear();
  interface_method_ids_.clear();
  runtime_type_ids_.clear();
  collection_type_ids_.clear();
  string_constant_ids_.clear();
  pending_constructors_.clear();
  field_initializers_.clear();
  BuildFunctionTable();
  BuildClassTable();
  if (declarations_ != nullptr) {
    for (const auto& [name, selector] : interface_method_ids_) {
      if (program_.selector_declarations.size() <= selector) {
        program_.selector_declarations.resize(static_cast<std::size_t>(selector) + 1);
      }
      program_.selector_declarations.at(selector) = declarations_->selectors.at(name);
    }
  }
  if (!model_.captured_mutables.empty()) {
    class_ids_.emplace("$Cell", static_cast<ClassId>(program_.classes.size()));
    program_.classes.push_back({
        .name = "$Cell",
        .field_order = {"value"},
        .field_types = {{.kind = frontend::TypeDescriptor::Kind::Class, .name = "$Cell"}},
        .callable = std::nullopt,
        .interfaces = {},
        .dispatch_targets = {},
        .kind = Class::Kind::CaptureCell,
        .callable_type = {},
        .declaration = {},
        .fields = {},
        .base_class = {},
    });
  }
  for (const frontend::Function& function : source_.functions) {
    if (function.role != frontend::Function::Role::RetiredEntry) {
      LowerFunction(function, model_.functions.at(function.name));
    }
  }
  for (const frontend::Class& owner : source_.classes) {
    for (const frontend::Function& method : owner.methods) {
      LowerFunction(method, model_.classes.at(owner.name).methods.at(method.name), &owner);
    }
  }
  for (const auto& owner : source_.classes) {
    LowerFieldDefaultInitializers(owner);
  }
  for (const auto& global : source_.globals) {
    LowerInitializer("$global." + global.name, *global.initializer,
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
      constructor.name = "$constructor";
      constructor.location = owner.location;
      LowerFunction(constructor, descriptor.constructor, &owner);
    }
  }
  RegisterBuiltinMembers();
  RecordProgramValueTypes();
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
  if (persistent_ != nullptr || native_mode_ == NativeProgramMode::Reloadable) {
    // 布局一旦用于实例就不能更换；首次接纳类型时必须绑定全部接口方法。
    for (const auto& interface : source_.interfaces) {
      for (const auto& method : interface.methods) {
        const auto key = interface.name + "." + method.name;
        const auto selector = persistent_ != nullptr
                                  ? persistent_->selectors.Get(key)
                                  : static_cast<std::uint32_t>(interface_method_ids_.size());
        interface_method_ids_.emplace(key, selector);
      }
    }
    return;
  }
  // 只为实际接口调用和绑定的方法生成 selector，未使用的接口不扩张分派表。
  for (const auto& [expression, member] : model_.interface_members) {
    interface_method_ids_.try_emplace(member.owner + "." + member.member, 0);
  }
  std::uint32_t selector = 0;
  for (const auto& interface : source_.interfaces) {
    for (const auto& method : interface.methods) {
      const auto selected = interface_method_ids_.find(interface.name + "." + method.name);
      if (selected != interface_method_ids_.end()) {
        selected->second = selector++;
      }
    }
  }
}

void Lowerer::BuildClassTable() {
  BuildInterfaceSelectors();
  for (const frontend::Class& source_class : source_.classes) {
    const auto class_info = model_.classes.find(source_class.name);
    if (class_info == model_.classes.end()) {
      continue;
    }
    const auto class_id = static_cast<ClassId>(program_.classes.size());
    class_ids_.emplace(source_class.name, class_id);
    program_.classes.emplace_back();
  }
  for (const auto& source_class : source_.classes) {
    const auto class_info = model_.classes.find(source_class.name);
    if (class_info == model_.classes.end()) {
      continue;
    }
    auto descriptor = BuildClass(source_class, class_info->second);
    program_.classes.at(class_ids_.at(source_class.name)) = std::move(descriptor);
    RegisterClassMembers(class_info->second);
  }
}

Class Lowerer::BuildClass(const frontend::Class& source_class,
                          const frontend::ClassDescriptor& class_info) {
  Class output_class;
  output_class.name = source_class.name;
  if (!source_.sources.empty() &&
      source_.sources.at(source_class.location.source_id).path == frontend::kErrorSourceName) {
    output_class.kind = Class::Kind::RuntimeError;
  }
  if (!source_.sources.empty() &&
      source_.sources.at(source_class.location.source_id).path == "builtin:DateParts") {
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
    const auto selected = interface_method_ids_.find(signature.first + '.' + signature.second);
    if (selected != interface_method_ids_.end()) {
      output_class.dispatch_targets.at(selected->second) =
          function_ids_.at(target.owner + '.' + target.member);
    }
  }
  output_class.field_order = class_info.field_order;
  if (declarations_ != nullptr) {
    const auto& declarations = declarations_->classes.at(source_class.name);
    output_class.declaration = declarations.declaration;
    if (class_info.base_class) {
      output_class.base_class = declarations_->nominals.at(*class_info.base_class);
    }
    for (const auto& name : output_class.field_order) {
      const auto identity = declarations.fields.at(name);
      const auto& field = class_info.fields.at(name);
      const auto initializer = field_initializers_.find(identity);
      output_class.fields.push_back({
          .declaration = identity,
          .type = RegisterRuntimeType(field.type),
          .visibility = field.visibility,
          .initializer = initializer == field_initializers_.end() ? kInvalidFunctionTarget
                                                                  : initializer->second,
      });
    }
  }
  output_class.field_types.reserve(output_class.field_order.size());
  for (const std::string& field_name : output_class.field_order) {
    const auto field = class_info.fields.find(field_name);
    output_class.field_types.push_back(field == class_info.fields.end() ? InvalidType()
                                                                        : field->second.type);
  }
  return output_class;
}

std::uint32_t Lowerer::AddFunctionId(const std::string& name, frontend::Function::Role role) {
  const auto& identity = role == frontend::Function::Role::InteractiveEntry ? "$entry" : name;
  const auto target = persistent_ == nullptr ? static_cast<std::uint32_t>(function_ids_.size())
                                             : persistent_->functions.Get(identity);
  function_ids_.emplace(name, target);
  return target;
}

void Lowerer::AddGlobalId(const std::string& name, GlobalKind kind) {
  const auto slot = persistent_ == nullptr
                        ? static_cast<std::uint32_t>(program_.global_initializers.size())
                        : persistent_->globals.Get(name);
  global_ids_.emplace(name, slot);
  const auto initializer = (kind == GlobalKind::StaticField ? "$static." : "$global.") + name;
  const auto function = AddFunctionId(initializer);
  if (program_.global_initializers.size() <= slot) {
    program_.global_initializers.resize(static_cast<std::size_t>(slot) + 1);
  }
  program_.global_initializers.at(slot) = function;
}

void Lowerer::BuildFunctionTable() {
  for (const auto& function : source_.functions) {
    if (function.role != frontend::Function::Role::RetiredEntry) {
      static_cast<void>(AddFunctionId(function.name, function.role));
    }
  }
  for (const frontend::Class& owner : source_.classes) {
    for (const frontend::Function& method : owner.methods) {
      const auto name = MethodKey(owner.name, method);
      static_cast<void>(AddFunctionId(name));
    }
    const auto name = owner.name + ".$constructor";
    if (native_mode_ == NativeProgramMode::Reloadable ||
        (persistent_ != nullptr && persistent_->functions.Has(name))) {
      static_cast<void>(AddFunctionId(name));
      pending_constructors_.push_back(&owner);
    }
  }
  for (const auto& owner : source_.classes) {
    for (const auto& field : owner.fields) {
      if (field.storage == frontend::MemberStorage::Static) {
        const auto key = owner.name + "." + field.name;
        AddGlobalId(key, GlobalKind::StaticField);
      } else if (native_mode_ == NativeProgramMode::Reloadable && field.initializer != nullptr) {
        const auto target = AddFunctionId("$field." + owner.name + '.' + field.name);
        field_initializers_.emplace(declarations_->classes.at(owner.name).fields.at(field.name),
                                    target);
      }
    }
  }
  for (const auto& global : source_.globals) {
    AddGlobalId(global.name, GlobalKind::Module);
  }
  lambda_ids_.clear();
  for (const auto& generated : model_.generated_functions) {
    static_cast<void>(AddFunctionId(generated.declaration->name));
  }
  for (const auto& lambda : model_.lambdas) {
    const auto target = persistent_ == nullptr
                            ? static_cast<std::uint32_t>(function_ids_.size())
                            : persistent_->functions.Get(lambda.expression->function->name);
    lambda_ids_.push_back(target);
    function_ids_.emplace("$lambda" + std::to_string(target), target);
  }
  program_.functions.resize(persistent_ == nullptr ? function_ids_.size()
                                                   : persistent_->functions.size());
}

void Lowerer::StoreFunction(Function function) {
  function.value_types.resize(function.register_count);
  if (function.signature) {
    const auto& signature = *function.signature;
    for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
      const auto& parameter = signature.parameters.at(index);
      auto type = program_.runtime_types.at(parameter.type);
      if (parameter.optional) {
        type = frontend::MakeNullableType(type);
      }
      function.value_types.at(index + signature.hidden_parameters) = type;
    }
  }
  const auto target = function_ids_.at(function.name);
  if (declarations_ != nullptr && function.identity.revision == 0) {
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
    const auto reg = AllocateTemporary(source.location);
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
  const auto* constructor =
      owner != nullptr && source.name == "$constructor" ? &model_.classes.at(owner->name) : nullptr;
  const auto masks = constructor == nullptr ? 0 : ConstructorMaskCount(*constructor);
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
    const auto type =
        function_info.parameters.at(function_info.parameter_indices.at(parameter.name)).type;
    const RegisterId reg = AllocateRegister(parameter.location);
    RecordValueType(reg, type);
  }
  if (constructor != nullptr) {
    // 初始化器只看声明处的全局环境；构造参数与 this 尚未发布到词法作用域。
    LowerConstructorInitializers(*constructor);
  }
  if (has_receiver) {
    DeclareLocal("this", 0, model_.classes.at(owner->name).type, source.location,
                 source.body.get());
  }
  // 完整参数窗口建立后，再为可捕获的参数分配共享槽位。
  for (std::size_t index = 0; index < source.parameters.size(); ++index) {
    const auto& parameter = source.parameters.at(index);
    const auto reg =
        static_cast<RegisterId>(index + ((has_receiver || lambda != nullptr) ? 1 : 0) + masks);
    DeclareLocal(
        parameter.name, reg,
        function_info.parameters.at(function_info.parameter_indices.at(parameter.name)).type,
        parameter.location, &parameter);
  }
  if (lambda != nullptr) {
    LowerCaptures(source, *lambda);
  }
}

void Lowerer::RegisterFunctionSignature(Function& function,
                                        const frontend::Function& source,
                                        const frontend::FunctionDescriptor& function_info) {
  function.signature.emplace();
  auto& signature = *function.signature;
  signature.hidden_parameters =
      static_cast<std::uint8_t>(function.parameter_count - source.parameters.size());
  for (const auto& parameter : source.parameters) {
    const auto& descriptor =
        function_info.parameters.at(function_info.parameter_indices.at(parameter.name));
    signature.parameters.push_back({
        .name = parameter.name,
        .type = RegisterRuntimeType(descriptor.type),
        .optional = function_info.optional_parameters.contains(parameter.name),
        .rest = descriptor.rest,
    });
  }
  if (function.return_type.kind != frontend::TypeDescriptor::Kind::Void) {
    signature.result = RegisterRuntimeType(function.return_type);
  }
}

Function Lowerer::PrepareFunction(const frontend::Function& source,
                                  const frontend::FunctionDescriptor& function_info,
                                  const frontend::Class* owner,
                                  const frontend::LambdaDescriptor* lambda) {
  Function function;
  function.is_generated_constructor =
      owner != nullptr && source.name == "$constructor" &&
      owner->constructor_kind == frontend::ConstructorKind::Generated;
  function.location = source.location;
  if (declarations_ != nullptr) {
    const auto identity = declarations_->functions.find(&source);
    if (identity != declarations_->functions.end()) {
      function.identity = identity->second;
    }
  }
  function.is_async = source.is_async;
  function.visibility = source.visibility;
  function.is_exported =
      owner == nullptr && lambda == nullptr &&
      (native_mode_ == NativeProgramMode::Reloadable || source.location.source_id == 0) &&
      source.export_kind == frontend::ExportKind::Exported;
  function.owner_class = owner == nullptr ? std::string() : owner->name;
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
  if (owner != nullptr && source.name == "$constructor") {
    function.return_type = {
        .kind = frontend::TypeDescriptor::Kind::Void,
        .name = frontend::kVoidTypeName,
    };
  }
  const bool has_receiver = owner != nullptr && source.storage == frontend::MemberStorage::Instance;
  const auto masks = owner != nullptr && source.name == "$constructor"
                         ? ConstructorMaskCount(model_.classes.at(owner->name))
                         : 0;
  function.parameter_count = static_cast<std::uint32_t>(
      source.parameters.size() + ((has_receiver || lambda != nullptr) ? 1 : 0) + masks);
  RegisterFunctionSignature(function, source, function_info);
  if (masks != 0) {
    auto& signature = *function.signature;
    signature.hidden_parameters = 1;
    const auto integer = RegisterRuntimeType(
        {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
    for (std::uint32_t index = 0; index < masks; ++index) {
      signature.parameters.push_back(
          {.name = "$provided" + std::to_string(index), .type = integer});
    }
  }
  return function;
}

void Lowerer::LowerFunction(const frontend::Function& source,
                            const frontend::FunctionDescriptor& function_info,
                            const frontend::Class* owner,
                            const frontend::LambdaDescriptor* lambda) {
  auto function = PrepareFunction(source, function_info, owner, lambda);
  if (source.intrinsic) {
    const auto builtin = *source.intrinsic;
    auto result = function.parameter_count;
    if (builtin == Builtin::DateToUTC) {
      function.instructions.push_back({
          .opcode = Opcode::LoadInt,
          .dst = result,
          .immediate = class_ids_.at("DateParts"),
          .location = source.location,
      });
      ++result;
    }
    function.instructions.push_back({
        .opcode = Opcode::BuiltinCall,
        .dst = result,
        .lhs = 0,
        .index = static_cast<std::uint32_t>(builtin),
        .location = source.location,
    });
    function.instructions.push_back(
        {.opcode = Opcode::Return, .dst = result, .location = source.location});
    function.register_count = result + 1;
    StoreFunction(std::move(function));
    return;
  }
  if (!source.native_symbol.empty()) {
    function.native_symbol = source.native_symbol;
    function.register_count = std::max(function.parameter_count, 1U);
    StoreFunction(std::move(function));
    return;
  }
  current_function_ = &function;
  binding_registers_.clear();
  next_register_ = 0;
  double_constant_ids_.clear();
  scopes_.clear();
  controls_.clear();
  finalizers_.clear();
  current_handler_.reset();
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
    const RegisterId result = AllocateTemporary(source.location);
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
      RecordValueType(value.reg, value.type);
      current_function_->value_types.at(value.reg) = value.type;
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
    const auto result = AllocateTemporary(expression.location);
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
    const auto boxed = AllocateTemporary(location);
    std::optional<std::size_t> null_jump;
    if (value.type.nullable) {
      Emit({.opcode = Opcode::LoadNull, .dst = boxed, .location = location});
      null_jump = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfNull, .lhs = value.reg, .location = location});
    }
    Emit({.opcode = Opcode::NewInstance, .dst = boxed, .index = layout, .location = location});
    Emit({.opcode = Opcode::SetField, .dst = boxed, .lhs = value.reg, .location = location});
    if (null_jump) {
      current_function_->instructions.at(*null_jump).index =
          static_cast<std::uint32_t>(current_function_->instructions.size());
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
  auto result = AllocateTemporary(location);
  Emit({
      .opcode = Opcode::CheckedCast,
      .dst = result,
      .lhs = value.reg,
      .index = index,
      .location = location,
  });
  if (target.kind == frontend::TypeDescriptor::Kind::Enum) {
    const auto boxed = result;
    result = AllocateTemporary(location);
    RecordValueType(result, target);
    std::optional<std::size_t> null_jump;
    if (target.nullable) {
      Emit({.opcode = Opcode::LoadNull, .dst = result, .location = location});
      null_jump = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfNull, .lhs = boxed, .location = location});
    }
    Emit({.opcode = Opcode::GetField, .dst = result, .lhs = boxed, .location = location});
    if (null_jump) {
      current_function_->instructions.at(*null_jump).index =
          static_cast<std::uint32_t>(current_function_->instructions.size());
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
      const auto reg = AllocateTemporary(expression.location);
      Emit({.opcode = Opcode::LoadNull, .dst = reg, .location = expression.location});
      return {
          .reg = reg,
          .type = {.kind = frontend::TypeDescriptor::Kind::Null, .name = frontend::kNullTypeName},
      };
    }
    case frontend::Expression::Kind::IntegerLiteral:
      return LowerIntegerLiteral(expression);
    case frontend::Expression::Kind::BooleanLiteral: {
      const RegisterId reg = AllocateTemporary(expression.location);
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
      return LowerDoubleLiteral(expression);
    case frontend::Expression::Kind::StringLiteral:
      return LowerStringLiteral(expression);
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
    case frontend::Expression::Kind::Call:
      return LowerCall(expression);
    case frontend::Expression::Kind::Member:
      return LowerMember(expression);
  }
  return {.reg = 0, .type = InvalidType()};
}

Lowerer::LoweredValue Lowerer::LowerAssignment(const frontend::Expression& expression) {
  if (expression.left->kind == frontend::Expression::Kind::Member) {
    const auto receiver = model_.expression_types.find(expression.left->object.get());
    if (receiver != model_.expression_types.end() &&
        receiver->second.kind == frontend::TypeDescriptor::Kind::Dynamic) {
      return LowerDynamicAssignment(expression);
    }
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
  AddDiagnostic(left.location, "left side of assignment is not lowerable");
  return {.reg = 0, .type = InvalidType()};
}

Lowerer::LoweredValue Lowerer::LowerName(const frontend::Expression& expression) {
  const Local* local = FindLocal(expression.text);
  if (local == nullptr && model_.globals.contains(expression.text)) {
    const auto result = AllocateTemporary(expression.location);
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
  const auto operand = LowerExpression(*expression.operand);
  const auto result = AllocateTemporary(expression.location);
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
  if (local == nullptr) {
    AddDiagnostic(left.location, "unknown local during assignment lowering");
    return {.reg = 0, .type = InvalidType()};
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
        .index = global_ids_.at(member->second.owner + "." + member->second.member),
        .location = left.location,
    });
    return value;
  }
  const LoweredValue object = SnapshotBefore(LowerExpression(*left.object), right);
  if (object.type.kind != frontend::TypeDescriptor::Kind::Class) {
    AddDiagnostic(left.location, "field assignment requires a class value");
    return {.reg = 0, .type = InvalidType()};
  }
  const frontend::ClassDescriptor* class_info = FindClass(object.type.name);
  if (class_info != nullptr) {
    const auto method = class_info->methods.find(left.text);
    if (method != class_info->methods.end() && method->second.is_setter) {
      const auto base = AllocateRegister(left.location);
      const auto argument = AllocateRegister(left.location);
      Emit({.opcode = Opcode::Move, .dst = base, .lhs = object.reg, .location = left.location});
      const auto value = LowerExpression(right);
      Emit({.opcode = Opcode::Move, .dst = argument, .lhs = value.reg, .location = right.location});
      const auto ignored = AllocateTemporary(left.location);
      const auto& method_owner = method->second.member_owner;
      Emit({
          .opcode = Opcode::Call,
          .dst = ignored,
          .lhs = base,
          .index = function_ids_.at(method_owner + "." + "$set." + left.text),
          .location = left.location,
      });
      return {.reg = value.reg, .type = method->second.setter_parameter_type.value_or(value.type)};
    }
  }
  const auto field = class_info == nullptr ? std::nullopt : FindField(*class_info, left.text);
  if (!field || class_info == nullptr) {
    AddDiagnostic(left.location, "unknown field during assignment lowering");
    return {.reg = 0, .type = InvalidType()};
  }
  const auto value = LowerExpression(right);
  Emit({
      .opcode = Opcode::SetField,
      .dst = object.reg,
      .lhs = value.reg,
      .index = *field,
      .location = left.location,
  });
  return {.reg = value.reg, .type = class_info->fields.at(left.text).type};
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
      expression.object->text == "super") {
    const auto member = model_.super_members.find(&expression);
    if (member != model_.super_members.end()) {
      const auto& signature =
          model_.classes.at(member->second.owner).methods.at(member->second.member);
      if (signature.is_getter) {
        const auto* receiver = FindLocal("this");
        if (receiver != nullptr) {
          const LoweredValue receiver_value{.reg = receiver->reg, .type = receiver->type};
          const std::string key = member->second.owner + ".$get." + member->second.member;
          return LowerInvocation(expression, signature, function_ids_.at(key), &receiver_value);
        }
      }
    }
  }
  return std::nullopt;
}

Lowerer::LoweredValue Lowerer::LowerMember(const frontend::Expression& expression) {
  const auto receiver_type = model_.expression_types.find(expression.object.get());
  if (receiver_type != model_.expression_types.end() &&
      receiver_type->second.kind == frontend::TypeDescriptor::Kind::Dynamic) {
    return LowerDynamicMember(expression);
  }
  if (const auto getter = LowerSuperGetter(expression)) {
    return *getter;
  }
  if (model_.interface_members.contains(&expression)) {
    return LowerFunctionValue(expression);
  }
  if (model_.instance_members.contains(&expression)) {
    return LowerFunctionValue(expression);
  }
  if (model_.builtin_method_values.contains(&expression) ||
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
    const auto result = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::GetGlobal,
        .dst = result,
        .index = global_ids_.at(member.owner + "." + member.member),
        .location = expression.location,
    });
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }
  if (model_.expression_types.at(&expression).kind == frontend::TypeDescriptor::Kind::Function) {
    const auto& object_type = model_.expression_types.at(expression.object.get());
    const auto name = object_type.nullable ? object_type.name.substr(0, object_type.name.size() - 1)
                                           : object_type.name;
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
  if (object.type.kind != frontend::TypeDescriptor::Kind::Class) {
    AddDiagnostic(expression.location, "member access requires a class value");
    return {.reg = 0, .type = InvalidType()};
  }
  const auto object_type =
      object.type.nullable
          ? model_.types.at(object.type.name.substr(0, object.type.name.size() - 1))
          : object.type;
  const frontend::ClassDescriptor* class_info = FindClass(object_type.name);
  const std::optional<FieldId> field =
      class_info == nullptr ? std::nullopt : FindField(*class_info, expression.text);
  if (!field.has_value() || class_info == nullptr) {
    if (class_info != nullptr) {
      const auto method = class_info->methods.find(expression.text);
      if (method != class_info->methods.end() && method->second.is_getter) {
        return LowerGetter(expression, object);
      }
    }
    AddDiagnostic(expression.location, "unknown field during lowering");
    return {.reg = 0, .type = InvalidType()};
  }
  const RegisterId result = AllocateTemporary(expression.location);
  std::optional<std::size_t> null_jump;
  if (expression.null_safe) {
    null_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfNull, .lhs = object.reg, .location = expression.location});
  }
  Emit({
      .opcode = Opcode::GetField,
      .dst = result,
      .lhs = object.reg,
      .index = *field,
      .location = expression.location,
  });
  FinishNullSafe(null_jump, result, expression.location);
  const auto type = expression.null_safe ? model_.expression_types.at(&expression)
                                         : class_info->fields.at(expression.text).type;
  return {.reg = result, .type = type};
}

Lowerer::LoweredValue Lowerer::LowerGetter(const frontend::Expression& expression,
                                           const LoweredValue& object) {
  const auto type = model_.types.at(object.type.name);
  const auto owner = type.nullable ? type.name.substr(0, type.name.size() - 1) : type.name;
  const auto& method_owner = model_.classes.at(owner).methods.at(expression.text).member_owner;
  const std::string method_key = method_owner + "." + "$get." + expression.text;
  const auto base = AllocateRegister(expression.location);
  Emit({.opcode = Opcode::Move, .dst = base, .lhs = object.reg, .location = expression.location});
  const auto result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::Call,
      .dst = result,
      .lhs = base,
      .index = function_ids_.at(method_key),
      .location = expression.location,
  });
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

Lowerer::LoweredValue Lowerer::LowerCall(const frontend::Expression& expression) {
  if (expression.callee->kind == frontend::Expression::Kind::Member) {
    const auto receiver_type = model_.expression_types.find(expression.callee->object.get());
    if (receiver_type != model_.expression_types.end() &&
        receiver_type->second.kind == frontend::TypeDescriptor::Kind::Dynamic) {
      return LowerDynamicMethodCall(expression);
    }
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
    return LowerInvocation(expression, signature,
                           function_ids_.at(member.owner + "." + member.member), nullptr);
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
    const auto result = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::SleepFuture,
        .dst = result,
        .lhs = delay.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }
  if (expression.callee->kind == frontend::Expression::Kind::Name &&
      expression.callee->text == "super") {
    const auto& owner = model_.classes.at(current_function_->owner_class);
    const auto& base = model_.classes.at(*owner.base_class);
    const auto* receiver = FindLocal("this");
    return LowerConstructorCall(expression.arguments, model_.argument_order.at(&expression),
                                expression.location, base, receiver->reg);
  }
  if (expression.callee->kind == frontend::Expression::Kind::Member &&
      expression.callee->object->kind == frontend::Expression::Kind::Name &&
      expression.callee->object->text == frontend::kFutureTypeName &&
      !model_.bindings.contains(expression.callee->object.get())) {
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
    const auto result = AllocateTemporary(expression.location);
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
                            : ReadLocal(*FindLocal("this"), callee.location);
  const auto& method = model_.classes.at(target.owner).methods.at(target.member);
  std::string method_key = target.owner + ".";
  if (method.is_getter) {
    method_key += "$get.";
  } else if (method.is_setter) {
    method_key += "$set.";
  }
  method_key += target.member;
  const auto method_id = function_ids_.at(method_key);
  if (!callee.null_safe) {
    return LowerInvocation(expression, method, method_id, &receiver);
  }
  const auto null_jump = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfNull, .lhs = receiver.reg, .location = expression.location});
  auto result = LowerInvocation(expression, method, method_id, &receiver);
  FinishNullSafe(null_jump, result.reg, expression.location);
  result.type = model_.expression_types.at(&expression);
  return result;
}

Lowerer::LoweredValue Lowerer::LowerFutureCall(const frontend::Expression& expression) {
  const auto& method = expression.callee->text;
  auto payload = LowerExpression(*expression.arguments.front().value);
  if (method == "value") {
    payload.reg =
        LowerFuturePayload(payload, model_.expression_types.at(&expression), expression.location);
  }
  const auto result = AllocateTemporary(expression.location);
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
        .index = AddSyncFutureFunction(model_.expression_types.at(&expression), payload.type),
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
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

std::uint32_t Lowerer::AddSyncFutureFunction(const frontend::TypeDescriptor& return_type,
                                             const frontend::TypeDescriptor& callback_type) {
  Function function;
  function.name = "$future.sync." + std::to_string(program_.functions.size());
  const auto index = AddFunctionId(function.name);
  program_.functions.resize(persistent_ == nullptr ? function_ids_.size()
                                                   : persistent_->functions.size());
  function.return_type = return_type;
  function.parameter_count = 1;
  function.register_count = 2;
  function.is_async = true;
  function.signature.emplace();
  function.signature->parameters.push_back(
      {.name = "callback", .type = RegisterRuntimeType(callback_type)});
  function.signature->result = RegisterRuntimeType(return_type);
  function.instructions = {
      {.opcode = Opcode::CallIndirect, .dst = 1, .lhs = 2, .rhs = 0, .index = 0, .location = {}},
      {.opcode = Opcode::Return, .dst = 1, .location = {}},
  };
  const auto& payload = model_.generic_arguments.at(return_type.name).front();
  function.value_types = {callback_type, payload};
  if (payload.kind == frontend::TypeDescriptor::Kind::Enum) {
    function.instructions.pop_back();
    if (payload.nullable) {
      function.instructions.push_back(
          {.opcode = Opcode::JumpIfNull, .lhs = 1, .index = 4, .location = {}});
    }
    function.instructions.push_back({
        .opcode = Opcode::NewInstance,
        .dst = 2,
        .index = RegisterEnumType(payload),
        .location = {},
    });
    function.instructions.push_back(
        {.opcode = Opcode::SetField, .dst = 2, .lhs = 1, .location = {}});
    function.instructions.push_back({.opcode = Opcode::Return, .dst = 2, .location = {}});
    if (payload.nullable) {
      function.instructions.at(1).index = static_cast<std::uint32_t>(function.instructions.size());
      function.instructions.push_back({.opcode = Opcode::Return, .dst = 1, .location = {}});
    }
    function.register_count = 3;
    function.value_types.push_back(
        {.kind = frontend::TypeDescriptor::Kind::Dynamic, .name = frontend::kDynamicTypeName});
  }
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
  if (declarations_ != nullptr) {
    const auto* member =
        expression.kind == frontend::Expression::Kind::Call ? expression.callee.get() : &expression;
    const auto super = model_.super_members.find(member);
    if (super != model_.super_members.end()) {
      const auto& self = FindLocal("this")->type;
      const auto& parent = *model_.classes.at(self.name).base_class;
      binding_receiver = declarations_->classes.at(parent).declaration;
    }
  }
  const RegisterId result = AllocateTemporary(expression.location);
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

RegisterId Lowerer::AllocateTemporary(frontend::SourceLocation location) {
  return AllocateRegister(location);
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

bool Lowerer::IsBoolean(const frontend::TypeDescriptor& type) {
  return type.kind == frontend::TypeDescriptor::Kind::Bool;
}

std::optional<FieldId> Lowerer::FindField(const frontend::ClassDescriptor& class_info,
                                          std::string_view name) {
  const auto field = class_info.fields.find(name);
  if (field == class_info.fields.end()) {
    return std::nullopt;
  }
  return field->second.index;
}

void Lowerer::Emit(Instruction instruction) {
  if (current_function_ != nullptr) {
    if (instruction.opcode == Opcode::GetIndex || instruction.opcode == Opcode::SetIndex ||
        (instruction.opcode == Opcode::BuiltinCall &&
         instruction.index == static_cast<std::uint32_t>(Builtin::ArrayPush))) {
      const auto array = instruction.opcode == Opcode::SetIndex ? instruction.dst : instruction.lhs;
      const auto type = current_function_->value_types.at(array);
      instruction.array_type = RegisterArrayType(type);
    }
    RecordInstructionValueType(instruction);
    instruction.handler = current_handler_;
    current_function_->instructions.push_back(instruction);
  }
}

void Lowerer::FinishNullSafe(std::optional<std::size_t> null_jump,
                             RegisterId result,
                             frontend::SourceLocation location) {
  if (!null_jump) {
    return;
  }
  const auto end_jump = current_function_->instructions.size();
  Emit({.opcode = Opcode::Jump, .location = location});
  current_function_->instructions.at(*null_jump).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
  Emit({.opcode = Opcode::LoadNull, .dst = result, .location = location});
  current_function_->instructions.at(end_jump).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
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
  RecordValueType(reg, type);
  current_function_->value_types.at(reg) = type;
  const auto binding = model_.bindings.find(declaration);
  const auto binding_id = binding == model_.bindings.end() ? 0 : binding->second;
  const bool cell =
      binding != model_.bindings.end() && model_.captured_mutables.contains(binding_id);
  if (cell) {
    const auto boxed = AllocateTemporary(location);
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
  if (!value) {
    AddDiagnostic(expression.location, "integer literal is out of range");
    return {.reg = 0, .type = InvalidType()};
  }
  const RegisterId reg = AllocateTemporary(expression.location);
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

Lowerer::LoweredValue Lowerer::LowerStringLiteral(const frontend::Expression& expression) {
  const auto& value = expression.text;
  const auto [found, inserted] =
      string_constant_ids_.try_emplace(value, static_cast<std::uint32_t>(program_.strings.size()));
  const auto index = found->second;
  if (inserted) {
    program_.strings.push_back(value);
  }
  const RegisterId reg = AllocateTemporary(expression.location);
  Emit({.opcode = Opcode::LoadString, .dst = reg, .index = index, .location = expression.location});
  return {
      .reg = reg,
      .type = {.kind = frontend::TypeDescriptor::Kind::String, .name = frontend::kStringTypeName},
  };
}

Lowerer::LoweredValue Lowerer::LowerEnumMember(const frontend::Expression& expression,
                                               const frontend::TypeDescriptor& type) {
  const auto ordinal = model_.enum_values.at(std::string(type.name) + "." + expression.text);
  const RegisterId reg = AllocateTemporary(expression.location);
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
  const auto result = AllocateTemporary(expression.location);
  Emit({
      .opcode = expression.text == "++" ? Opcode::AddInt : Opcode::SubInt,
      .dst = result,
      .lhs = operand.reg,
      .rhs = one,
      .location = expression.location,
  });
  RegisterId returned = expression.postfix ? AllocateTemporary(expression.location) : result;
  if (expression.postfix) {
    Emit({
        .opcode = Opcode::Move,
        .dst = returned,
        .lhs = operand.reg,
        .location = expression.location,
    });
  }
  StoreMutationTarget(target, destination, result, expression.location);
  return {.reg = returned, .type = operand.type};
}

Lowerer::MutationTarget Lowerer::LowerMutationTarget(const frontend::Expression& target) {
  MutationTarget destination;
  const auto static_member = model_.static_members.find(&target);
  if (static_member != model_.static_members.end()) {
    const auto& member = static_member->second;
    destination.global = global_ids_.at(member.owner + "." + member.member);
    destination.value = LowerMember(target);
    RecordValueType(destination.value.reg, destination.value.type);
    return destination;
  }
  if (target.kind == frontend::Expression::Kind::Member) {
    auto receiver = LowerExpression(*target.object);
    receiver.reg =
        EmitTemporary({.opcode = Opcode::Move, .lhs = receiver.reg, .location = target.location});
    if (receiver.type.kind == frontend::TypeDescriptor::Kind::Dynamic) {
      frontend::Expression name;
      name.text = target.text;
      name.location = target.location;
      const auto text = LowerStringLiteral(name);
      const auto value = AllocateTemporary(target.location);
      const std::array values{receiver.reg, text.reg};
      EmitBuiltinCall(Builtin::DynamicMemberGet, value, values, target.location);
      destination.object = receiver.reg;
      destination.member = text.reg;
      destination.value = {.reg = value, .type = model_.expression_types.at(&target)};
      return destination;
    }
    const auto* owner = FindClass(receiver.type.name);
    if (owner == nullptr) {
      AddDiagnostic(target.location, "increment target is not a class field");
      return destination;
    }
    destination.field = FindField(*owner, target.text);
    if (!destination.field) {
      const auto method = owner->methods.find(target.text);
      if (method != owner->methods.end() && method->second.is_getter && method->second.is_setter) {
        const auto& method_owner = method->second.member_owner;
        destination.object = receiver.reg;
        destination.setter = function_ids_.at(method_owner + ".$set." + target.text);
        destination.value = LowerGetter(target, receiver);
        return destination;
      }
      AddDiagnostic(target.location, "increment target field is unknown");
      return destination;
    }
    destination.object = receiver.reg;
    destination.value = {
        .reg = EmitTemporary({
            .opcode = Opcode::GetField,
            .lhs = receiver.reg,
            .index = *destination.field,
            .location = target.location,
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
    destination.object = receiver.reg;
    destination.index = position.reg;
    const auto value = AllocateTemporary(target.location);
    EmitIndexRead(value, receiver, position.reg, target.location);
    destination.value = {.reg = value, .type = model_.expression_types.at(&target)};
    return destination;
  }
  destination.value = LowerExpression(target);
  return destination;
}

void Lowerer::StoreMutationTarget(const frontend::Expression& target,
                                  const MutationTarget& destination,
                                  RegisterId value,
                                  frontend::SourceLocation location) {
  if (destination.global) {
    Emit({
        .opcode = Opcode::SetGlobal,
        .lhs = value,
        .index = *destination.global,
        .location = location,
    });
    return;
  }
  if (destination.setter) {
    if (!destination.object) {
      std::unreachable();
    }
    const auto base = AllocateRegister(location);
    const auto argument = AllocateRegister(location);
    Emit({
        .opcode = Opcode::Move,
        .dst = base,
        .lhs = *destination.object,
        .location = location,
    });
    Emit({.opcode = Opcode::Move, .dst = argument, .lhs = value, .location = location});
    const auto ignored = AllocateTemporary(location);
    Emit({
        .opcode = Opcode::Call,
        .dst = ignored,
        .lhs = base,
        .index = *destination.setter,
        .location = location,
    });
    return;
  }
  if (target.kind == frontend::Expression::Kind::Name) {
    const auto* local = FindLocal(target.text);
    if (local != nullptr) {
      Emit({
          .opcode = local->cell ? Opcode::SetField : Opcode::Move,
          .dst = local->reg,
          .lhs = value,
          .location = location,
      });
    } else if (model_.globals.contains(target.text)) {
      Emit({
          .opcode = Opcode::SetGlobal,
          .lhs = value,
          .index = global_ids_.at(target.text),
          .location = location,
      });
    }
    return;
  }
  if (destination.object && destination.field) {
    Emit({
        .opcode = Opcode::SetField,
        .dst = *destination.object,
        .lhs = value,
        .index = *destination.field,
        .location = location,
    });
    return;
  }
  if (destination.object && destination.member) {
    const std::array values{*destination.object, *destination.member, value};
    const auto result = AllocateTemporary(location);
    EmitBuiltinCall(Builtin::DynamicMemberSet, result, values, location);
    return;
  }
  if (destination.object && destination.index) {
    const LoweredValue receiver{
        .reg = *destination.object,
        .type = model_.expression_types.at(target.object.get()),
    };
    EmitIndexWrite(receiver, *destination.index, value, location);
  }
}

Lowerer::LoweredValue Lowerer::LowerUnary(const frontend::Expression& expression) {
  if (expression.text == "++" || expression.text == "--") {
    return LowerIncrement(expression);
  }
  const LoweredValue operand = LowerExpression(*expression.operand);
  const RegisterId result = AllocateTemporary(expression.location);
  if (expression.text == "-" && operand.type.kind == frontend::TypeDescriptor::Kind::Dynamic) {
    const std::array values{operand.reg};
    EmitBuiltinCall(Builtin::DynamicNegate, result, values, expression.location);
    return {.reg = result, .type = operand.type};
  }
  if (expression.text == "-" && operand.type.kind == frontend::TypeDescriptor::Kind::Double) {
    Emit({
        .opcode = Opcode::NegDouble,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  if (expression.text == "-" && IsInteger(operand.type)) {
    Emit({
        .opcode = Opcode::NegInt,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  if (expression.text == "!" && IsBoolean(operand.type)) {
    Emit({
        .opcode = Opcode::NotBool,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  if (expression.text == "~" && IsInteger(operand.type)) {
    Emit({
        .opcode = Opcode::BitNotInt,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  AddDiagnostic(expression.location, "unsupported unary operation");
  return {.reg = result, .type = InvalidType()};
}

Lowerer::LoweredValue Lowerer::SnapshotBefore(LoweredValue value,
                                              const frontend::Expression& later) {
  if (HasAssignments(later)) {
    const auto snapshot = AllocateTemporary(later.location);
    Emit({.opcode = Opcode::Move, .dst = snapshot, .lhs = value.reg, .location = later.location});
    value.reg = snapshot;
  }
  return value;
}

Lowerer::LoweredValue Lowerer::LowerBinary(const frontend::Expression& expression) {
  if (expression.text == "=") {
    return LowerAssignment(expression);
  }
  if (const auto operation = frontend::CompoundAssignmentOperation(expression.text)) {
    return LowerCompoundAssignment(expression, *operation);
  }
  if (expression.text == "??") {
    const auto left = LowerExpression(*expression.left);
    return LowerBranchValue(
        expression, Opcode::JumpIfNull, left.reg, [&] { return left; },
        [&] { return LowerExpression(*expression.right); });
  }
  if (expression.text == "&&" || expression.text == "||") {
    return LowerLogical(expression);
  }
  if (expression.text == "+" &&
      model_.expression_types.at(&expression).kind == frontend::TypeDescriptor::Kind::Int &&
      expression.left != nullptr && expression.left->kind == frontend::Expression::Kind::Binary &&
      expression.left->text == "+") {
    return LowerIntegerAdditionChain(expression);
  }
  return LowerBinaryValue(expression);
}

Lowerer::LoweredValue Lowerer::LowerIntegerAdditionChain(const frontend::Expression& expression) {
  std::vector<const frontend::Expression*> rights;
  const frontend::Expression* leftmost = &expression;
  while (leftmost->left != nullptr && leftmost->left->kind == frontend::Expression::Kind::Binary &&
         leftmost->left->text == "+") {
    rights.push_back(leftmost->right.get());
    leftmost = leftmost->left.get();
  }
  rights.push_back(leftmost->right.get());
  auto value = LowerExpression(*leftmost->left);
  for (const auto* right_expression : std::views::reverse(rights)) {
    const auto right = LowerExpression(*right_expression);
    const auto result = AllocateTemporary(expression.location);
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

Lowerer::LoweredValue Lowerer::LowerCompoundAssignment(const frontend::Expression& expression,
                                                       std::string_view operation) {
  const auto& left = *expression.left;
  if (model_.expression_types.at(&left).kind == frontend::TypeDescriptor::Kind::Dynamic) {
    const auto opcode = SelectBinaryOpcode(
        operation, {.kind = frontend::TypeDescriptor::Kind::Int, .name = frontend::kIntTypeName});
    if (!opcode) {
      AddDiagnostic(expression.location, "unsupported compound assignment operation");
      return {.reg = 0, .type = InvalidType()};
    }
    return LowerDynamicCompoundAssignment(expression, *opcode);
  }
  auto target = LowerMutationTarget(left);
  target.value = SnapshotBefore(target.value, *expression.right);
  const auto right = LowerExpression(*expression.right);
  const auto opcode = SelectBinaryOpcode(operation, target.value.type);
  if (!opcode) {
    AddDiagnostic(expression.location, "unsupported compound assignment operation");
    return {.reg = 0, .type = InvalidType()};
  }
  const auto result = EmitTemporary({
      .opcode = *opcode,
      .lhs = target.value.reg,
      .rhs = right.reg,
      .location = expression.location,
  });
  StoreMutationTarget(left, target, result, expression.location);
  return {.reg = result, .type = target.value.type};
}

Lowerer::LoweredValue Lowerer::LowerBinaryValue(const frontend::Expression& expression) {
  const auto left = SnapshotBefore(LowerExpression(*expression.left), *expression.right);
  const LoweredValue right = LowerExpression(*expression.right);
  using Kind = frontend::TypeDescriptor::Kind;
  if (left.type.kind == Kind::Dynamic || right.type.kind == Kind::Dynamic) {
    const auto operation =
        SelectBinaryOpcode(expression.text, {.kind = Kind::Int, .name = frontend::kIntTypeName});
    if (!operation) {
      AddDiagnostic(expression.location, "unsupported binary operation");
      return {.reg = 0, .type = InvalidType()};
    }
    const auto selector = EmitTemporary({
        .opcode = Opcode::LoadInt,
        .immediate = static_cast<std::int64_t>(*operation),
        .location = expression.location,
    });
    const auto result = AllocateTemporary(expression.location);
    const std::array values{selector, left.reg, right.reg};
    EmitBuiltinCall(Builtin::DynamicBinary, result, values, expression.location);
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }
  if (expression.text == "+" && model_.expression_types.at(&expression).kind == Kind::String) {
    const auto result = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::StringConcat,
        .dst = result,
        .lhs = left.reg,
        .rhs = right.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = {.kind = Kind::String, .name = frontend::kStringTypeName}};
  }
  if ((expression.text == "==" || expression.text == "!=") &&
      (left.type.nullable || right.type.nullable || left.type.kind == Kind::Null ||
       right.type.kind == Kind::Null || left.type.kind == Kind::Class ||
       left.type.kind == Kind::Interface || left.type.kind == Kind::Function ||
       left.type.kind == Kind::ByteArray || left.type.kind == Kind::ByteData ||
       left.type.kind == Kind::RegExp || left.type.kind == Kind::RegExpMatch)) {
    const auto result = AllocateTemporary(expression.location);
    Emit({
        .opcode = expression.text == "==" ? Opcode::EqualValue : Opcode::NotEqualValue,
        .dst = result,
        .lhs = left.reg,
        .rhs = right.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = {.kind = Kind::Bool, .name = frontend::kBoolTypeName}};
  }
  const auto opcode = SelectBinaryOpcode(expression.text, left.type);
  if (!opcode) {
    AddDiagnostic(expression.location, "unsupported binary operation");
    return {.reg = 0, .type = InvalidType()};
  }
  const auto result = EmitTemporary({
      .opcode = *opcode,
      .lhs = left.reg,
      .rhs = right.reg,
      .location = expression.location,
  });
  return {.reg = result, .type = model_.expression_types.at(&expression)};
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
    const auto reg = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::LoadBool,
        .dst = reg,
        .immediate = expression.text == "||" ? 1 : 0,
        .location = expression.location,
    });
    return {
        .reg = reg,
        .type = {.kind = frontend::TypeDescriptor::Kind::Bool, .name = frontend::kBoolTypeName},
    };
  };
  if (expression.text == "&&") {
    return LowerBranchValue(expression, Opcode::JumpIfFalse, left.reg, right, constant);
  }
  return LowerBranchValue(expression, Opcode::JumpIfFalse, left.reg, constant, right);
}

}  // namespace vela::ir
