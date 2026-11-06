#include "compiler/frontend/modules/module_binding.h"
#include "base/declaration.h"
#include "compiler/frontend/modules/source_file.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/builtins/builtins.h"
#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"

namespace vela::frontend {
namespace {

struct Symbol final {
  std::string target;
  ExportKind export_kind = ExportKind::Local;
};

struct ModuleScope final {
  absl::flat_hash_map<std::string, Symbol> declarations;
  absl::flat_hash_map<std::string, std::string> names;
  absl::flat_hash_map<std::string, std::uint32_t> prefixes;
};

class ModuleBinding final {
 public:
  ModuleBinding(std::vector<SourceModule> modules, std::vector<SourceFile> sources)
      : modules_(std::move(modules)), sources_(std::move(sources)), scopes_(modules_.size()) {}

  ParseResult Run();
  Diagnostics BindStandard(Program& program);

 private:
  void Error(SourceLocation location, std::string message);
  template <class Declaration>
  void Declare(const Declaration& declaration, DeclarationKind kind, std::size_t index);
  void CollectDeclarations();
  void BindImports();
  void AddImport(const Program::Import& imported, std::uint32_t dependency, ModuleScope& scope);
  void BindProgram(Program& program);
  void BindClass(Class& declaration, const std::string& owner);
  void BindInterface(Interface& declaration, const std::string& owner);
  void BindFunction(Function& function, const std::string& owner);
  // 先登记整个参数列表再绑定上界，允许上界引用列表中后出现的参数。
  void BeginTypeParameters(std::vector<TypeParameter>& parameters, const std::string& owner);
  void EndTypeParameters();
  void BeginScope();
  void EndScope();
  void DeclareLocal(std::string_view name);
  void BindType(TypeName& type);
  void BindExpression(Expression* expression);
  void BindStatement(Statement* statement);
  void BindScoped(Statement* statement);
  [[nodiscard]] bool IsLocal(const std::string& name) const;
  [[nodiscard]] const TypeParameter* FindTypeParameter(std::string_view name) const;
  std::string Resolve(const std::string& name, SourceLocation location);
  std::string ExportedName(std::uint32_t dependency,
                           const std::string& name,
                           SourceLocation location);

  std::vector<SourceModule> modules_;
  std::vector<SourceFile> sources_;
  std::vector<ModuleScope> scopes_;
  Diagnostics diagnostics_;
  std::uint32_t current_ = 0;
  std::vector<std::vector<std::string_view>> locals_;
  absl::flat_hash_map<std::string_view, std::uint32_t> local_bindings_;
  // 参数节点由正在绑定的声明拥有；作用域只在绑定该声明期间借用。
  std::vector<absl::flat_hash_map<std::string_view, const TypeParameter*>> type_parameters_;
  absl::flat_hash_map<std::string_view, const TypeParameter*> type_parameter_bindings_;
};

void ModuleBinding::Error(SourceLocation location, std::string message) {
  diagnostics_.push_back({
      .location = location,
      .message = std::move(message),
      .source_path = sources_.at(location.source_id).path,
  });
}

template <class Declaration>
void ModuleBinding::Declare(const Declaration& declaration,
                            DeclarationKind kind,
                            std::size_t index) {
  auto& scope = scopes_.at(index);
  auto& module = modules_.at(index);
  if (IsBuiltinName(declaration.name) || IsStandardName(declaration.name)) {
    Error(declaration.location, "declaration name is reserved for a builtin");
  }
  const auto binding_id = module.binding_id;
  const auto target = index == 0 ? declaration.name
                                 : "$module" + std::to_string(binding_id) + "." + declaration.name;
  if (!scope.declarations
           .emplace(declaration.name,
                    Symbol{.target = target, .export_kind = declaration.export_kind})
           .second) {
    const auto* category = kind == DeclarationKind::Function ? "function" : "module declaration";
    Error(declaration.location,
          "duplicate " + std::string(category) + " '" + declaration.name + "'");
  }
  scope.names.try_emplace(declaration.name, target);
  // 类型参数的身份与上界在绑定声明后写入，来源只记录已解析的形式。
  module.program.origins.emplace(target, DeclarationOrigin{
                                             .module_uri = sources_.at(index).path,
                                             .name = declaration.name,
                                             .kind = kind,
                                             .arguments = {},
                                             .type_parameters = {},
                                             .members = {},
                                             .generic_signature = {},
                                         });
}

void ModuleBinding::CollectDeclarations() {
  for (std::size_t index = 0; index < modules_.size(); ++index) {
    const auto& program = modules_.at(index).program;
    for (const auto& declaration : program.functions) {
      Declare(declaration, DeclarationKind::Function, index);
    }
    for (const auto& declaration : program.classes) {
      Declare(declaration, DeclarationKind::Class, index);
    }
    for (const auto& declaration : program.enums) {
      Declare(declaration, DeclarationKind::Enum, index);
    }
    for (const auto& declaration : program.interfaces) {
      Declare(declaration, DeclarationKind::Interface, index);
    }
    for (const auto& declaration : program.aliases) {
      Declare(declaration, DeclarationKind::Alias, index);
    }
    for (const auto& declaration : program.globals) {
      Declare(declaration, DeclarationKind::Global, index);
    }
  }
}

void ModuleBinding::AddImport(const Program::Import& imported,
                              std::uint32_t dependency,
                              ModuleScope& scope) {
  if (!imported.alias.empty()) {
    if (IsBuiltinName(imported.alias) || IsStandardName(imported.alias) ||
        scope.names.contains(imported.alias) ||
        !scope.prefixes.emplace(imported.alias, dependency).second) {
      Error(imported.location, "duplicate or reserved import prefix '" + imported.alias + "'");
    }
    return;
  }
  for (const auto& [name, symbol] : scopes_.at(dependency).declarations) {
    if (symbol.export_kind != ExportKind::Exported) {
      continue;
    }
    const auto [found, inserted] = scope.names.try_emplace(name, symbol.target);
    if (scope.prefixes.contains(name) || (!inserted && found->second != symbol.target)) {
      Error(imported.location, "conflicting imported name '" + name + "'; use an import prefix");
    }
  }
}

void ModuleBinding::BindImports() {
  for (std::size_t index = 0; index < modules_.size(); ++index) {
    const auto& module = modules_.at(index);
    auto& scope = scopes_.at(index);
    for (std::size_t imported = 0; imported < module.program.imports.size(); ++imported) {
      AddImport(module.program.imports.at(imported), module.dependencies.at(imported), scope);
    }
  }
}

bool ModuleBinding::IsLocal(const std::string& name) const {
  return local_bindings_.contains(name);
}

const TypeParameter* ModuleBinding::FindTypeParameter(std::string_view name) const {
  const auto found = type_parameter_bindings_.find(name);
  return found == type_parameter_bindings_.end() ? nullptr : found->second;
}

void ModuleBinding::BeginScope() {
  locals_.emplace_back();
}

void ModuleBinding::EndScope() {
  for (const auto name : locals_.back()) {
    const auto found = local_bindings_.find(name);
    if (--found->second == 0) {
      local_bindings_.erase(found);
    }
  }
  locals_.pop_back();
}

void ModuleBinding::DeclareLocal(std::string_view name) {
  locals_.back().push_back(name);
  ++local_bindings_[name];
}

void ModuleBinding::BeginTypeParameters(std::vector<TypeParameter>& parameters,
                                        const std::string& owner) {
  auto& scope = type_parameters_.emplace_back();
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    auto& parameter = parameters.at(index);
    parameter.identity = {.owner = owner, .index = static_cast<std::uint32_t>(index)};
    const auto [previous, inserted] = scope.try_emplace(parameter.name, nullptr);
    if (!inserted) {
      Error(parameter.location, "duplicate generic parameter '" + parameter.name + "'");
      continue;
    }
    auto& current = type_parameter_bindings_[parameter.name];
    previous->second = current;
    current = &parameter;
  }
  for (auto& parameter : parameters) {
    if (parameter.bound) {
      BindType(*parameter.bound);
    }
  }
}

void ModuleBinding::EndTypeParameters() {
  for (const auto& [name, previous] : type_parameters_.back()) {
    if (previous == nullptr) {
      type_parameter_bindings_.erase(name);
    } else {
      type_parameter_bindings_.at(name) = previous;
    }
  }
  type_parameters_.pop_back();
}

std::string ModuleBinding::ExportedName(std::uint32_t dependency,
                                        const std::string& name,
                                        SourceLocation location) {
  const auto& declarations = scopes_.at(dependency).declarations;
  const auto found = declarations.find(name);
  if (found == declarations.end() || found->second.export_kind != ExportKind::Exported) {
    Error(location, "module does not export '" + name + "'");
    return name;
  }
  return found->second.target;
}

std::string ModuleBinding::Resolve(const std::string& name, SourceLocation location) {
  const auto& scope = scopes_.at(current_);
  const auto separator = name.find('.');
  if (separator != std::string::npos) {
    const auto prefix = scope.prefixes.find(std::string_view(name).substr(0, separator));
    if (prefix != scope.prefixes.end()) {
      return ExportedName(prefix->second, name.substr(separator + 1), location);
    }
  }
  const auto found = scope.names.find(name);
  if (found != scope.names.end()) {
    return found->second;
  }
  if (current_ != 0 && scopes_.front().declarations.contains(name)) {
    // 入口声明保留宿主可见名称；合并 AST 不能令其他模块越过各自的 import 作用域。
    Error(location, "name '" + name + "' is not declared or imported by this module");
  }
  // 未绑定的未限定名字交由类型检查器按表达式/类型上下文诊断，避免模块绑定抢占其语义。
  return name;
}

void ModuleBinding::BindType(TypeName& type) {
  if (type.kind == TypeName::Kind::Inferred) {
    return;
  }
  for (auto& argument : type.arguments) {
    BindType(argument);
  }
  if (type.kind != TypeName::Kind::Named && type.kind != TypeName::Kind::Generic) {
    return;
  }
  if (const auto* parameter = FindTypeParameter(type.name)) {
    if (type.kind == TypeName::Kind::Generic) {
      Error(type.location, "generic parameter '" + type.name + "' does not accept type arguments");
      return;
    }
    type.kind = TypeName::Kind::Parameter;
    type.parameter = parameter->identity;
    return;
  }
  // 类型参数在其作用域内遮蔽同名导入前缀；参数没有可限定访问的成员类型。
  if (const auto separator = type.name.find('.');
      separator != std::string::npos &&
      (FindTypeParameter(std::string_view(type.name).substr(0, separator)) != nullptr)) {
    Error(type.location, "generic parameter '" + type.name.substr(0, separator) +
                             "' does not declare member types");
    return;
  }
  type.name = Resolve(type.name, type.location);
}

void ModuleBinding::BindExpression(Expression* expression) {
  if (expression == nullptr) {
    return;
  }
  if (expression->kind == Expression::Kind::Member &&
      expression->object->kind == Expression::Kind::Name && !IsLocal(expression->object->text)) {
    const auto& prefixes = scopes_.at(current_).prefixes;
    const auto prefix = prefixes.find(expression->object->text);
    if (prefix != prefixes.end()) {
      expression->text = ExportedName(prefix->second, expression->text, expression->location);
      expression->kind = Expression::Kind::Name;
      expression->object.reset();
    }
  } else if (expression->kind == Expression::Kind::Name && !IsLocal(expression->text)) {
    if (scopes_.at(current_).prefixes.contains(expression->text)) {
      Error(expression->location, "an import prefix must be followed by an exported member");
    } else if (FindTypeParameter(expression->text) != nullptr) {
      // 上界只提供静态能力，不提供构造或静态成员；需要实例时显式传入工厂函数。
      Error(expression->location, "generic parameter '" + expression->text +
                                      "' is not a value; pass a function such as () => " +
                                      expression->text);
    } else {
      expression->text = Resolve(expression->text, expression->location);
    }
  }
  BindType(expression->type);
  for (auto& argument : expression->type_arguments) {
    BindType(argument);
  }
  for (auto* child : ExpressionOperands(*expression)) {
    BindExpression(child);
  }
  for (auto& element : expression->elements) {
    BindExpression(element.get());
  }
  for (auto& entry : expression->entries) {
    BindExpression(entry.key.get());
    BindExpression(entry.value.get());
  }
  for (auto& argument : expression->arguments) {
    BindExpression(argument.value.get());
  }
  for (auto& arm : expression->arms) {
    BindExpression(arm.pattern.get());
    BeginScope();
    if (arm.kind == MatchArm::Kind::Binding) {
      DeclareLocal(arm.binding);
    }
    BindExpression(arm.value.get());
    EndScope();
  }
  if (expression->function) {
    BindFunction(*expression->function, {});
  }
}

void ModuleBinding::BindScoped(Statement* statement) {
  BeginScope();
  BindStatement(statement);
  EndScope();
}

void ModuleBinding::BindStatement(Statement* statement) {
  if (statement == nullptr) {
    return;
  }
  const bool scope = statement->IntroducesScope();
  if (scope) {
    BeginScope();
  }
  BindType(statement->type);
  BindStatement(statement->initializer.get());
  BindExpression(statement->expression.get());
  BindExpression(statement->condition.get());
  BindExpression(statement->iterable.get());
  if (statement->kind == Statement::Kind::ForEach) {
    DeclareLocal(statement->name);
  }
  BindScoped(statement->then_branch.get());
  BindScoped(statement->else_branch.get());
  BindScoped(statement->finally_branch.get());
  for (auto& clause : statement->catches) {
    BeginScope();
    if (!clause.name.empty()) {
      BindType(clause.type);
      DeclareLocal(clause.name);
    }
    BindStatement(clause.body.get());
    EndScope();
  }
  BindExpression(statement->increment.get());
  for (auto& child : statement->statements) {
    BindStatement(child.get());
  }
  for (auto& branch : statement->cases) {
    BindExpression(branch.label.get());
    BindScoped(branch.body.get());
  }
  if (statement->kind == Statement::Kind::VariableDeclaration) {
    DeclareLocal(statement->name);
  }
  if (scope) {
    EndScope();
  }
}

void ModuleBinding::BindFunction(Function& function, const std::string& owner) {
  BeginTypeParameters(function.type_parameters, owner);
  BeginScope();
  BindType(function.return_type);
  for (auto& parameter : function.parameters) {
    BindType(parameter.type);
    DeclareLocal(parameter.name);
  }
  BindStatement(function.body.get());
  EndScope();
  EndTypeParameters();
}

void ModuleBinding::BindClass(Class& declaration, const std::string& owner) {
  BeginTypeParameters(declaration.type_parameters, owner);
  if (declaration.base_class) {
    BindType(*declaration.base_class);
  }
  for (auto& interface : declaration.interfaces) {
    BindType(interface);
  }
  for (auto& field : declaration.fields) {
    BindType(field.type);
    BindExpression(field.initializer.get());
  }
  for (auto& method : declaration.methods) {
    BindFunction(method, owner + '.' + method.name);
  }
  if (declaration.constructor) {
    BindFunction(*declaration.constructor, {});
  }
  EndTypeParameters();
}

void ModuleBinding::BindInterface(Interface& declaration, const std::string& owner) {
  BeginTypeParameters(declaration.type_parameters, owner);
  for (auto& method : declaration.methods) {
    BindFunction(method, owner + '.' + method.name);
  }
  EndTypeParameters();
}

void ModuleBinding::BindProgram(Program& program) {
  const auto target = [&](const auto& declaration) -> std::string {
    return scopes_.at(current_).declarations.at(declaration.name).target;
  };
  const auto rename = [&](auto& declaration) -> void {
    declaration.name = target(declaration);
    if constexpr (requires { declaration.type_parameters; }) {
      program.origins.at(declaration.name).type_parameters = declaration.type_parameters;
    }
  };
  for (auto& function : program.functions) {
    BindFunction(function, target(function));
    rename(function);
  }
  for (auto& declaration : program.classes) {
    BindClass(declaration, target(declaration));
    rename(declaration);
  }
  for (auto& declaration : program.enums) {
    rename(declaration);
  }
  for (auto& declaration : program.interfaces) {
    BindInterface(declaration, target(declaration));
    rename(declaration);
  }
  for (auto& alias : program.aliases) {
    BeginTypeParameters(alias.type_parameters, target(alias));
    BindType(alias.target);
    EndTypeParameters();
    rename(alias);
  }
  for (auto& global : program.globals) {
    BindType(global.type);
    BindExpression(global.initializer.get());
    rename(global);
  }
}

ParseResult ModuleBinding::Run() {
  CollectDeclarations();
  if (diagnostics_.empty()) {
    BindImports();
  }
  Program combined;
  const auto can_bind = diagnostics_.empty();
  for (auto& module : modules_) {
    combined.import_sources.push_back(std::move(module.dependencies));
    // 声明与导入表已经建立；函数体中的未知名字不能阻止后续依赖绑定其声明身份。
    if (can_bind) {
      BindProgram(module.program);
    }
    AppendProgram(combined, std::move(module.program));
    ++current_;
  }
  combined.imports.clear();
  combined.sources = std::move(sources_);
  return {.program = std::move(combined), .diagnostics = std::move(diagnostics_)};
}

Diagnostics ModuleBinding::BindStandard(Program& program) {
  for (auto& declaration : program.classes) {
    BindClass(declaration, declaration.name);
  }
  return std::move(diagnostics_);
}

}  // namespace

ParseResult BindModules(std::vector<SourceModule> modules, std::vector<SourceFile> sources) {
  return ModuleBinding(std::move(modules), std::move(sources)).Run();
}

Diagnostics BindStandardDeclarations(Program& program, SourceFile source) {
  std::vector<SourceModule> modules(1);
  std::vector<SourceFile> sources;
  sources.push_back(std::move(source));
  return ModuleBinding(std::move(modules), std::move(sources)).BindStandard(program);
}

}  // namespace vela::frontend
