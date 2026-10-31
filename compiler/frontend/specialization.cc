#include "compiler/frontend/specialization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/declaration.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/ast_clone.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/standard_environment.h"
#include "compiler/frontend/type_limits.h"
#include "compiler/frontend/type_names.h"

namespace vela::frontend {
namespace {

bool IsBuiltinGenericName(std::string_view name) {
  return name == kSetTypeName || name == kMapTypeName || name == kFutureTypeName ||
         name == kCompleterTypeName;
}

bool IsBuiltinGenericExpression(const Expression& expression, const Expression* object) {
  if (IsBuiltinGenericName(expression.text)) {
    return true;
  }
  return expression.kind == Expression::Kind::Member && object != nullptr &&
         object->kind == Expression::Kind::Name && object->text == kFutureTypeName;
}

bool IsTypeWithinLimit(const TypeName& type, std::size_t depth = 1) {
  if (depth > kMaxTypeNesting) {
    return false;
  }
  return std::ranges::all_of(type.arguments, [&](const TypeName& child) -> bool {
    return IsTypeWithinLimit(child, depth + 1);
  });
}

// 替换参数后合并由参数带入的可空；源码直接书写的双重可空保留给类型检查诊断。
bool SubstitutesNullableElement(const TypeName& source, const TypeSubstitutions& substitutions) {
  return source.kind == TypeName::Kind::Nullable &&
         source.arguments.front().kind == TypeName::Kind::Parameter &&
         substitutions.contains(source.arguments.front().parameter);
}

std::vector<Function> ExtractMethodTemplates(std::vector<Function>& methods) {
  std::vector<Function> concrete;
  std::vector<Function> templates;
  for (auto& method : methods) {
    (method.type_parameters.empty() ? concrete : templates).push_back(std::move(method));
  }
  methods = std::move(concrete);
  return templates;
}

}  // namespace

std::string SpecializationName(std::string_view base, std::span<const TypeName> arguments) {
  std::string result(base);
  result += '<';
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += CanonicalTypeText(arguments.subspan(index).front());
  }
  return result + '>';
}

void Specializer::Error(SourceLocation location, std::string message) {
  diagnostics_.push_back({
      .location = location,
      .message = std::move(message),
      .source_path = location.source_id < program_.sources.size()
                         ? program_.sources.at(location.source_id).path
                         : std::string(),
  });
}

TypeSubstitutions Specializer::Bind(std::span<const TypeParameter> parameters,
                                    std::span<const TypeName> arguments,
                                    SourceLocation location) {
  TypeSubstitutions result;
  if (parameters.size() != arguments.size()) {
    Error(location, "generic argument count does not match type parameter count");
    return result;
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    result.emplace(parameters.subspan(index).front().identity, arguments.subspan(index).front());
  }
  return result;
}

void Specializer::RenameMethodParameters(std::span<const Function> methods,
                                         const std::string& owner,
                                         TypeSubstitutions& substitutions) {
  for (const auto& method : methods) {
    for (std::size_t index = 0; index < method.type_parameters.size(); ++index) {
      const auto& parameter = method.type_parameters.at(index);
      auto renamed = ParameterTypeName(parameter, parameter.location);
      renamed.parameter = {
          .owner = owner + '.' + method.name,
          .index = static_cast<std::uint32_t>(index),
      };
      substitutions.emplace(parameter.identity, std::move(renamed));
    }
  }
}

void Specializer::AssignMethodParameters(std::vector<Function>& methods, const std::string& owner) {
  for (auto& method : methods) {
    for (std::size_t index = 0; index < method.type_parameters.size(); ++index) {
      method.type_parameters.at(index).identity = {
          .owner = owner + '.' + method.name,
          .index = static_cast<std::uint32_t>(index),
      };
    }
  }
}

void Specializer::RecordApplication(const std::string& name,
                                    const std::vector<TypeName>& arguments,
                                    SourceLocation location) {
  applications_.push_back({.name = name, .arguments = arguments, .location = location});
}

std::string Specializer::Instantiate(const std::string& name,
                                     const std::vector<TypeName>& arguments,
                                     SourceLocation location) {
  const auto key = SpecializationName(name, arguments);
  if (instantiated_.contains(key)) {
    return key;
  }
  RecordApplication(name, arguments, location);
  // 限制单条依赖链的展开深度，不限制项目中彼此独立的具体特化数量。
  if (expansion_depth_ >= kMaxGenericExpansionDepth) {
    Error(location, "generic expansion depth exceeds the limit of 128; check expanding recursion");
    return key;
  }
  const bool abstract = std::ranges::any_of(arguments, HasTypeParameters);
  const auto function = function_templates_.find(name);
  // 泛型函数的抽象应用只需要替换后的签名，由类型检查器直接求解，不产生声明。
  if (abstract && function != function_templates_.end()) {
    return key;
  }
  instantiated_.insert(key);
  expansion_depths_.emplace(key, expansion_depth_ + 1);
  generic_types_.emplace(key, TypeName{
                                  .name = name,
                                  .location = location,
                                  .kind = TypeName::Kind::Generic,
                                  .arguments = arguments,
                                  .parameter = {},
                              });
  if (!abstract) {
    if (const auto origin = program_.origins.find(name); origin != program_.origins.end()) {
      auto concrete_origin = origin->second;
      concrete_origin.arguments = arguments;
      concrete_origin.type_parameters.clear();
      program_.origins.emplace(key, std::move(concrete_origin));
    }
  }
  // 模板表在 Collect 后不再插入或改写，队列借用的节点地址保持稳定。
  // 只排入模板引用；取出队列项时融合克隆与规范化，不沿泛型调用链递归展开 C++ 栈。
  if (function != function_templates_.end()) {
    Function declaration;
    declaration.name = key;
    functions_.push_back({
        .declaration = std::move(declaration),
        .substitutions = Bind(function->second->type_parameters, arguments, location),
        .source = function->second,
        .expansion_depth = expansion_depth_ + 1,
        .abstract = nullptr,
    });
    work_.push_back({.kind = Work::Kind::Function, .index = functions_.size() - 1});
  } else if (const auto owner = class_templates_.find(name); owner != class_templates_.end()) {
    auto* source = owner->second;
    Class* application = nullptr;
    if (abstract) {
      application =
          program_.templates.class_applications.emplace_back(std::make_unique<Class>()).get();
    }
    Class declaration;
    declaration.name = key;
    auto substitutions = Bind(source->type_parameters, arguments, location);
    RenameMethodParameters(source->method_templates, key, substitutions);
    classes_.push_back({
        .declaration = std::move(declaration),
        .substitutions = std::move(substitutions),
        .source = source,
        .expansion_depth = expansion_depth_ + 1,
        .abstract = application,
    });
    if (application != nullptr) {
      application->name = key;
    }
    work_.push_back({.kind = Work::Kind::Class, .index = classes_.size() - 1});
  } else if (const auto interface = interface_templates_.find(name);
             interface != interface_templates_.end()) {
    auto* source = interface->second;
    Interface* application = nullptr;
    if (abstract) {
      application =
          program_.templates.interface_applications.emplace_back(std::make_unique<Interface>())
              .get();
    }
    Interface declaration;
    declaration.name = key;
    auto substitutions = Bind(source->type_parameters, arguments, location);
    RenameMethodParameters(source->method_templates, key, substitutions);
    interfaces_.push_back({
        .declaration = std::move(declaration),
        .substitutions = std::move(substitutions),
        .source = source,
        .expansion_depth = expansion_depth_ + 1,
        .abstract = application,
    });
    if (application != nullptr) {
      application->name = key;
    }
    work_.push_back({.kind = Work::Kind::Interface, .index = interfaces_.size() - 1});
  } else {
    Error(location, "unknown generic declaration '" + name + "'");
  }
  return key;
}

bool Specializer::IsShadowed(const std::string& name) const {
  return std::ranges::any_of(scopes_ | std::views::reverse,
                             [&](const auto& scope) -> bool { return scope.contains(name); });
}

bool Specializer::IsGenericDeclaration(std::string_view name) const {
  return function_templates_.contains(name) || class_templates_.contains(name) ||
         interface_templates_.contains(name);
}

void Specializer::Normalize(TypeName& type, const TypeSubstitutions& substitutions) {
  if (type_depth_ >= kMaxTypeNesting) {
    Error(type.location,
          "type nesting exceeds the limit of 128; check expanding generic recursion");
    return;
  }
  ++type_depth_;
  NormalizeType(type, substitutions);
  if (type_depth_ == 1 && diagnostics_.empty() && !IsTypeWithinLimit(type)) {
    Error(type.location,
          "type nesting exceeds the limit of 128; check expanding generic recursion");
  }
  --type_depth_;
}

void Specializer::NormalizeType(TypeName& type, const TypeSubstitutions& substitutions) {
  if (type.kind == TypeName::Kind::Parameter) {
    if (const auto found = substitutions.find(type.parameter); found != substitutions.end()) {
      const auto location = type.location;
      type = found->second;
      // 方法参数换名保留引用位置，具体实参沿用书写实参的位置。
      if (type.kind == TypeName::Kind::Parameter) {
        type.location = location;
      }
    }
    return;
  }
  const bool collapse = SubstitutesNullableElement(type, substitutions);
  for (auto& argument : type.arguments) {
    Normalize(argument, substitutions);
  }
  if (collapse && IsNullableTypeName(type.arguments.front())) {
    type = TypeName(type.arguments.front());
    return;
  }
  NormalizeTypeHead(type);
}

void Specializer::NormalizeTypeHead(TypeName& type) {
  const auto alias = aliases_.find(type.name);
  if ((type.kind == TypeName::Kind::Named || type.kind == TypeName::Kind::Generic) &&
      alias != aliases_.end()) {
    const auto key = SpecializationName(type.name, type.arguments);
    if (!active_aliases_.insert(key).second) {
      Error(type.location, "cyclic or expanding type alias '" + type.name + "'");
      return;
    }
    if (!alias->second.type_parameters.empty()) {
      RecordApplication(type.name, type.arguments, type.location);
    }
    const auto bound = Bind(alias->second.type_parameters, type.arguments, type.location);
    type = CloneNormalizedType(alias->second.target, bound);
    active_aliases_.erase(key);
    return;
  }
  if (type.kind == TypeName::Kind::Generic) {
    // 内建泛型由类型检查器直接解析；含类型参数的应用留给定义检查建立抽象成员签名。
    if (IsBuiltinGenericName(type.name) || std::ranges::any_of(type.arguments, HasTypeParameters)) {
      return;
    }
    type.name = Instantiate(type.name, type.arguments, type.location);
    type.kind = TypeName::Kind::Named;
    type.arguments.clear();
  } else if (type.kind == TypeName::Kind::Array || type.kind == TypeName::Kind::Nullable ||
             type.kind == TypeName::Kind::Function) {
    type.name = type.ToString();
  }
}

TypeName Specializer::CloneNormalizedType(const TypeName& source,
                                          const TypeSubstitutions& substitutions) {
  if (type_depth_ >= kMaxTypeNesting) {
    Error(source.location,
          "type nesting exceeds the limit of 128; check expanding generic recursion");
    return source;
  }
  ++type_depth_;
  TypeName result;
  if (source.kind == TypeName::Kind::Parameter) {
    const auto found = substitutions.find(source.parameter);
    result = found == substitutions.end() ? source : found->second;
    if (result.kind == TypeName::Kind::Parameter) {
      result.location = source.location;
    }
  } else {
    result.name = source.name;
    result.location = source.location;
    result.kind = source.kind;
    result.arguments.reserve(source.arguments.size());
    for (const auto& argument : source.arguments) {
      result.arguments.push_back(CloneNormalizedType(argument, substitutions));
    }
    if (SubstitutesNullableElement(source, substitutions) &&
        IsNullableTypeName(result.arguments.front())) {
      result = TypeName(result.arguments.front());
    } else {
      NormalizeTypeHead(result);
    }
  }
  if (type_depth_ == 1 && diagnostics_.empty() && !IsTypeWithinLimit(result)) {
    Error(result.location,
          "type nesting exceeds the limit of 128; check expanding generic recursion");
  }
  --type_depth_;
  return result;
}

TypeName Specializer::TransformType(const TypeName& source) {
  return CloneNormalizedType(source, *clone_substitutions_);
}

void Specializer::TransformApplication(Expression& target, const Expression& source) {
  NormalizeExpressionHead(target, *clone_substitutions_, source.object.get());
}

void Specializer::BeginScope() {
  scopes_.emplace_back();
}

void Specializer::EndScope() {
  scopes_.pop_back();
}

void Specializer::Declare(const std::string& name) {
  if (!scopes_.empty()) {
    scopes_.back().insert(name);
  }
}

void Specializer::NormalizeExpressionHead(Expression& expression,
                                          const TypeSubstitutions& substitutions,
                                          const Expression* object) {
  if (expression.type_arguments.empty()) {
    return;
  }
  if (expression.kind == Expression::Kind::Name && IsShadowed(expression.text)) {
    Error(expression.location, "type arguments cannot be applied to a local binding");
    return;
  }
  // 方法应用和含类型参数的应用依赖接收者或抽象环境，由类型检查器在调用点求解。
  if ((expression.kind == Expression::Kind::Member &&
       !IsBuiltinGenericExpression(expression, object)) ||
      std::ranges::any_of(expression.type_arguments, HasTypeParameters)) {
    return;
  }
  if (aliases_.contains(expression.text)) {
    TypeName type{
        .name = expression.text,
        .location = expression.location,
        .kind = TypeName::Kind::Generic,
        .arguments = expression.type_arguments,
        .parameter = {},
    };
    Normalize(type, substitutions);
    if (type.kind == TypeName::Kind::Named ||
        (type.kind == TypeName::Kind::Generic &&
         (type.name == kSetTypeName || type.name == kMapTypeName))) {
      expression.text = type.name;
      expression.type_arguments = type.arguments;
    } else {
      Error(expression.location, "type alias does not name a constructible type");
    }
  } else if (IsGenericDeclaration(expression.text)) {
    expression.text = Instantiate(expression.text, expression.type_arguments, expression.location);
    expression.type_arguments.clear();
  } else if (!IsBuiltinGenericExpression(expression, object)) {
    Error(expression.location, "type arguments require a generic declaration");
  }
}

void Specializer::Normalize(Expression* expression, const TypeSubstitutions& substitutions) {
  if (expression == nullptr) {
    return;
  }
  if (!expression->type.name.empty()) {
    Normalize(expression->type, substitutions);
  }
  for (auto& argument : expression->type_arguments) {
    Normalize(argument, substitutions);
  }
  NormalizeExpressionHead(*expression, substitutions, expression->object.get());
  for (auto* child : ExpressionOperands(*expression)) {
    Normalize(child, substitutions);
  }
  for (auto& element : expression->elements) {
    Normalize(element.get(), substitutions);
  }
  for (auto& argument : expression->arguments) {
    Normalize(argument.value.get(), substitutions);
  }
  for (auto& arm : expression->arms) {
    scopes_.emplace_back();
    if (arm.kind == MatchArm::Kind::Binding) {
      scopes_.back().insert(arm.binding);
    }
    Normalize(arm.pattern.get(), substitutions);
    Normalize(arm.value.get(), substitutions);
    scopes_.pop_back();
  }
  if (expression->function) {
    Normalize(*expression->function, substitutions);
  }
}

void Specializer::Normalize(Statement* statement, const TypeSubstitutions& substitutions) {
  if (statement == nullptr) {
    return;
  }
  const bool has_scope = statement->IntroducesScope();
  if (has_scope) {
    scopes_.emplace_back();
  }
  if (!statement->type.name.empty()) {
    Normalize(statement->type, substitutions);
  }
  Normalize(statement->initializer.get(), substitutions);
  Normalize(statement->expression.get(), substitutions);
  Normalize(statement->condition.get(), substitutions);
  Normalize(statement->iterable.get(), substitutions);
  if (statement->kind == Statement::Kind::ForEach) {
    scopes_.back().insert(statement->name);
    if (statement->destructuring) {
      scopes_.back().insert(statement->second_name);
    }
  }
  NormalizeScoped(statement->then_branch.get(), substitutions);
  NormalizeScoped(statement->else_branch.get(), substitutions);
  NormalizeScoped(statement->finally_branch.get(), substitutions);
  for (auto& clause : statement->catches) {
    scopes_.emplace_back();
    if (!clause.name.empty()) {
      Normalize(clause.type, substitutions);
      scopes_.back().insert(clause.name);
    }
    Normalize(clause.body.get(), substitutions);
    scopes_.pop_back();
  }
  Normalize(statement->increment.get(), substitutions);
  for (auto& child : statement->statements) {
    Normalize(child.get(), substitutions);
  }
  for (auto& branch : statement->cases) {
    Normalize(branch.label.get(), substitutions);
    Normalize(branch.body.get(), substitutions);
  }
  if (statement->kind == Statement::Kind::VariableDeclaration && !scopes_.empty()) {
    scopes_.back().insert(statement->name);
  }
  if (has_scope) {
    scopes_.pop_back();
  }
}

void Specializer::NormalizeScoped(Statement* statement, const TypeSubstitutions& substitutions) {
  if (statement != nullptr) {
    scopes_.emplace_back();
    Normalize(statement, substitutions);
    scopes_.pop_back();
  }
}

void Specializer::Normalize(Function& function, const TypeSubstitutions& substitutions) {
  scopes_.emplace_back();
  for (auto& parameter : function.type_parameters) {
    if (parameter.bound) {
      Normalize(*parameter.bound, substitutions);
    }
  }
  Normalize(function.return_type, substitutions);
  for (auto& parameter : function.parameters) {
    Normalize(parameter.type, substitutions);
    scopes_.back().insert(parameter.name);
  }
  Normalize(function.body.get(), substitutions);
  scopes_.pop_back();
}

void Specializer::Normalize(Class& owner, const TypeSubstitutions& substitutions) {
  if (owner.base_class) {
    Normalize(*owner.base_class, substitutions);
  }
  for (auto& interface : owner.interfaces) {
    Normalize(interface, substitutions);
  }
  for (auto& field : owner.fields) {
    Normalize(field.type, substitutions);
    Normalize(field.initializer.get(), substitutions);
  }
  for (auto& method : owner.methods) {
    Normalize(method, substitutions);
  }
  for (auto& method : owner.method_templates) {
    Normalize(method, substitutions);
  }
  if (owner.constructor) {
    Normalize(*owner.constructor, substitutions);
  }
}

void Specializer::CollectDeclarations() {
  absl::flat_hash_set<std::string> names;
  const auto declare = [&](const auto& declaration, const std::string& kind) -> void {
    if (!names.insert(declaration.name).second) {
      Error(declaration.location, "duplicate " + kind + " '" + declaration.name + "'");
    }
  };
  for (const auto& declaration : program_.functions) {
    declare(declaration, "function");
  }
  for (const auto& declaration : program_.classes) {
    declare(declaration, "type");
  }
  for (const auto& declaration : program_.enums) {
    declare(declaration, "type");
  }
  for (const auto& declaration : program_.interfaces) {
    declare(declaration, "interface");
  }
  for (const auto& declaration : program_.aliases) {
    declare(declaration, "type alias");
  }
  for (const auto& declaration : program_.globals) {
    declare(declaration, "global");
  }
}

void Specializer::CollectAliases() {
  for (auto& alias : program_.aliases) {
    if (IsBuiltinName(alias.name)) {
      Error(alias.location, "type alias name is reserved for a builtin");
    }
    if (!aliases_.emplace(alias.name, alias).second) {
      Error(alias.location, "duplicate type alias '" + alias.name + "'");
    }
  }
}

void Specializer::CollectFunctionTemplates() {
  for (auto& function : program_.functions) {
    auto& origin = program_.origins.at(function.name);
    DescribeParameters(origin, function.type_parameters);
    if (!function.type_parameters.empty()) {
      origin.generic_signature = DescribeFunctionContract(function);
    }
    if (IsBuiltinName(function.name)) {
      Error(function.location, "function name is reserved for a builtin");
    }
    if (function.type_parameters.empty()) {
      functions_.push_back({
          .declaration = std::move(function),
          .substitutions = {},
          .source = nullptr,
          .expansion_depth = 0,
          .abstract = nullptr,
      });
      work_.push_back({.kind = Work::Kind::Function, .index = functions_.size() - 1});
    } else {
      program_.templates.functions.push_back(std::move(function));
    }
  }
  program_.functions.clear();
  for (auto& function : program_.templates.functions) {
    function_templates_.emplace(function.name, &function);
  }
}

void Specializer::CollectClassTemplates() {
  for (auto& owner : program_.classes) {
    if (IsBuiltinName(owner.name) && !IsStandardTypeNamespace(program_, owner)) {
      Error(owner.location, "type name is reserved for a builtin");
    }
    owner.method_templates = ExtractMethodTemplates(owner.methods);
    DescribeClassTemplate(owner);
    if (owner.type_parameters.empty()) {
      classes_.push_back({
          .declaration = std::move(owner),
          .substitutions = {},
          .source = nullptr,
          .expansion_depth = 0,
          .abstract = nullptr,
      });
      work_.push_back({.kind = Work::Kind::Class, .index = classes_.size() - 1});
    } else {
      program_.templates.classes.push_back(std::move(owner));
    }
  }
  program_.classes.clear();
  for (auto& owner : program_.templates.classes) {
    class_templates_.emplace(owner.name, &owner);
  }
}

void Specializer::CollectInterfaceTemplates() {
  for (auto& interface : program_.interfaces) {
    interface.method_templates = ExtractMethodTemplates(interface.methods);
    DescribeInterfaceTemplate(interface);
    if (interface.type_parameters.empty()) {
      interfaces_.push_back({
          .declaration = std::move(interface),
          .substitutions = {},
          .source = nullptr,
          .expansion_depth = 0,
          .abstract = nullptr,
      });
      work_.push_back({.kind = Work::Kind::Interface, .index = interfaces_.size() - 1});
    } else {
      program_.templates.interfaces.push_back(std::move(interface));
    }
  }
  program_.interfaces.clear();
  for (auto& interface : program_.templates.interfaces) {
    interface_templates_.emplace(interface.name, &interface);
  }
}

void Specializer::NormalizeAliases() {
  std::vector<Program::TypeAlias> concrete;
  for (auto& alias : program_.aliases) {
    DescribeParameters(program_.origins.at(alias.name), alias.type_parameters);
    (alias.type_parameters.empty() ? concrete : program_.templates.aliases)
        .push_back(std::move(alias));
  }
  program_.aliases = std::move(concrete);
  for (auto& alias : program_.aliases) {
    Normalize(alias.target, {});
  }
}

Diagnostics Specializer::Initialize() {
  CollectDeclarations();
  CollectAliases();
  CollectFunctionTemplates();
  CollectClassTemplates();
  CollectInterfaceTemplates();
  NormalizeAliases();
  for (auto& global : program_.globals) {
    if (!global.type.name.empty()) {
      Normalize(global.type, {});
    }
    Normalize(global.initializer.get(), {});
  }
  Expand();
  Publish();
  expansion_depth_ = 0;
  return TakeDiagnostics();
}

Diagnostics Specializer::Retain(std::span<const GenericSpecialization> retained) {
  for (const auto& request : retained) {
    if (request.method) {
      continue;
    }
    auto arguments = request.arguments;
    for (auto& argument : arguments) {
      Normalize(argument, {});
    }
    (void)Instantiate(request.name, arguments, {});
  }
  Expand();
  Publish();
  expansion_depth_ = 0;
  return TakeDiagnostics();
}

void Specializer::Expand() {
  // 深度优先处理依赖，保证指数分叉的展开递归先触达深度边界，而非耗尽整个宽度。
  while (diagnostics_.empty() && !work_.empty()) {
    const auto work = work_.back();
    work_.pop_back();
    if (work.kind == Work::Kind::Function) {
      ExpandFunction(functions_.at(work.index));
    } else if (work.kind == Work::Kind::Class) {
      ExpandClass(classes_.at(work.index));
    } else if (work.kind == Work::Kind::Interface) {
      ExpandInterface(interfaces_.at(work.index));
    } else {
      ExpandFunction(methods_.at(work.index).function);
    }
  }
}

void Specializer::ExpandFunction(Pending<Function>& pending) {
  expansion_depth_ = pending.expansion_depth;
  if (pending.source == nullptr) {
    Normalize(pending.declaration, pending.substitutions);
    return;
  }
  const auto name = pending.declaration.name;
  auto application = std::move(pending.declaration.application);
  clone_substitutions_ = &pending.substitutions;
  pending.declaration = CloneFunction(*pending.source, *this);
  clone_substitutions_ = nullptr;
  pending.declaration.name = name;
  pending.declaration.type_parameters.clear();
  pending.declaration.application = std::move(application);
}

void Specializer::ExpandClass(Pending<Class>& pending) {
  expansion_depth_ = pending.expansion_depth;
  auto& target = pending.abstract != nullptr ? *pending.abstract : pending.declaration;
  class_declarations_.insert_or_assign(target.name, &target);
  if (pending.source == nullptr) {
    Normalize(target, pending.substitutions);
    return;
  }
  const auto name = target.name;
  clone_substitutions_ = &pending.substitutions;
  // 抽象应用只提供成员签名；方法体由模板自身的定义检查覆盖，不复制以免重复诊断。
  target = pending.abstract != nullptr ? CloneClassDeclaration(*pending.source, *this)
                                       : CloneClass(*pending.source, *this);
  clone_substitutions_ = nullptr;
  target.name = name;
  target.type_parameters.clear();
  AssignMethodParameters(target.method_templates, name);
}

void Specializer::ExpandInterface(Pending<Interface>& pending) {
  expansion_depth_ = pending.expansion_depth;
  auto& target = pending.abstract != nullptr ? *pending.abstract : pending.declaration;
  interface_declarations_.insert_or_assign(target.name, &target);
  if (pending.source != nullptr) {
    const auto name = target.name;
    clone_substitutions_ = &pending.substitutions;
    target = CloneInterface(*pending.source, *this);
    clone_substitutions_ = nullptr;
    target.name = name;
    target.type_parameters.clear();
    AssignMethodParameters(target.method_templates, name);
    return;
  }
  for (auto& method : target.methods) {
    Normalize(method, pending.substitutions);
  }
  for (auto& method : target.method_templates) {
    Normalize(method, pending.substitutions);
  }
}

void Specializer::Publish() {
  for (auto& method : methods_) {
    auto& declaration = method.function.declaration;
    if (method.kind == DeclarationKind::Interface) {
      interface_declarations_.at(method.owner)->methods.push_back(std::move(declaration));
    } else {
      class_declarations_.at(method.owner)->methods.push_back(std::move(declaration));
    }
  }
  for (auto& function : functions_) {
    program_.functions.push_back(std::move(function.declaration));
  }
  for (auto& owner : classes_) {
    if (owner.abstract == nullptr) {
      program_.classes.push_back(std::move(owner.declaration));
    }
  }
  for (auto& interface : interfaces_) {
    if (interface.abstract == nullptr) {
      program_.interfaces.push_back(std::move(interface.declaration));
    }
  }
  functions_.clear();
  classes_.clear();
  interfaces_.clear();
  methods_.clear();
  for (auto& owner : program_.classes) {
    class_declarations_.insert_or_assign(owner.name, &owner);
  }
  for (auto& owner : program_.interfaces) {
    interface_declarations_.insert_or_assign(owner.name, &owner);
  }
}

const Function* Specializer::FunctionTemplate(std::string_view name) const {
  const auto found = function_templates_.find(name);
  return found == function_templates_.end() ? nullptr : found->second;
}

const Class* Specializer::ClassTemplate(std::string_view name) const {
  const auto found = class_templates_.find(name);
  return found == class_templates_.end() ? nullptr : found->second;
}

const Interface* Specializer::InterfaceTemplate(std::string_view name) const {
  const auto found = interface_templates_.find(name);
  return found == interface_templates_.end() ? nullptr : found->second;
}

const Program::TypeAlias* Specializer::AliasTemplate(std::string_view name) const {
  const auto found = aliases_.find(name);
  return found == aliases_.end() || found->second.type_parameters.empty() ? nullptr
                                                                          : &found->second;
}

TypeName Specializer::ExpandAlias(TypeName type) {
  const auto alias = aliases_.find(type.name);
  if ((type.kind == TypeName::Kind::Named || type.kind == TypeName::Kind::Generic) &&
      alias != aliases_.end()) {
    const auto key = SpecializationName(type.name, type.arguments);
    if (!active_aliases_.insert(key).second || active_aliases_.size() > kMaxTypeNesting) {
      Error(type.location, "cyclic or expanding type alias '" + type.name + "'");
      return type;
    }
    if (!alias->second.type_parameters.empty()) {
      RecordApplication(type.name, type.arguments, type.location);
    }
    const auto bound = Bind(alias->second.type_parameters, type.arguments, type.location);
    type = ExpandAlias(SubstituteTypeName(alias->second.target, bound));
    active_aliases_.erase(key);
    return type;
  }
  for (auto& argument : type.arguments) {
    argument = ExpandAlias(std::move(argument));
  }
  return type;
}

TypeName Specializer::DescribeType(TypeName type) const {
  if (type.kind == TypeName::Kind::Named) {
    if (const auto found = generic_types_.find(type.name); found != generic_types_.end()) {
      type = found->second;
    }
  }
  for (auto& argument : type.arguments) {
    argument = DescribeType(std::move(argument));
  }
  return type;
}

std::string Specializer::Specialize(const std::string& name,
                                    const std::vector<TypeName>& arguments,
                                    SourceLocation location) {
  const auto depth = expansion_depth_;
  auto normalized = arguments;
  for (auto& argument : normalized) {
    Normalize(argument, {});
  }
  const auto result = Instantiate(name, normalized, location);
  Expand();
  expansion_depth_ = depth;
  return result;
}

Diagnostics Specializer::TakeDiagnostics() {
  return std::exchange(diagnostics_, {});
}

std::vector<Specializer::Application> Specializer::TakeApplications() {
  return std::exchange(applications_, {});
}

absl::flat_hash_map<std::string, TypeName> Specializer::TakeTypeApplications() {
  return std::exchange(generic_types_, {});
}

const void* Specializer::FunctionSource(std::size_t index) const {
  return functions_.at(index).source;
}

const void* Specializer::ClassSource(std::size_t index) const {
  return classes_.at(index).source;
}

const Function& Specializer::function(std::size_t index) const {
  return functions_.at(index).declaration;
}

const Class& Specializer::class_declaration(std::size_t index) const {
  const auto& pending = classes_.at(index);
  return pending.abstract != nullptr ? *pending.abstract : pending.declaration;
}

const Interface& Specializer::interface_declaration(std::size_t index) const {
  const auto& pending = interfaces_.at(index);
  return pending.abstract != nullptr ? *pending.abstract : pending.declaration;
}

bool Specializer::IsAbstractClass(std::size_t index) const {
  return classes_.at(index).abstract != nullptr;
}

bool Specializer::IsAbstractInterface(std::size_t index) const {
  return interfaces_.at(index).abstract != nullptr;
}

void Specializer::SetExpansionOwner(std::string_view name) {
  const auto found = expansion_depths_.find(name);
  expansion_depth_ = found == expansion_depths_.end() ? 0 : found->second;
}

}  // namespace vela::frontend
