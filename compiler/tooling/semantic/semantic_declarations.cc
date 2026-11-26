#include "compiler/tooling/semantic/semantic_index.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/builtin.h"

#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/tooling/documentation/builtin_documentation.h"
#include "compiler/tooling/documentation/documentation.h"
#include "compiler/tooling/semantic/semantic_syntax.h"

namespace vela::tooling {
namespace {
using frontend::Expression;
using frontend::TypeName;

std::optional<std::size_t> FindNamedSymbol(
    const absl::flat_hash_map<std::string, std::size_t>* symbols,
    std::string_view name) {
  if (symbols != nullptr) {
    const auto found = symbols->find(name);
    if (found != symbols->end()) {
      return found->second;
    }
  }
  return std::nullopt;
}

const Expression* ConstructorDefault(const frontend::ClassDescriptor& owner, std::size_t index) {
  const auto& binding = owner.constructor_bindings[index];
  if (binding.base_parameter) {
    return nullptr;
  }
  const auto* initial =
      binding.declaration != nullptr ? binding.declaration->initializer.get() : nullptr;
  if (initial != nullptr || binding.field == nullptr) {
    return initial;
  }
  return binding.field->declaration->initializer.get();
}

void AppendConstructorParameter(std::string& text, std::string_view name, std::string_view type) {
  if (text.back() != '(') {
    text += ", ";
  }
  text += name;
  text += ": ";
  text += type;
}

std::string TypeText(const TypeName& type) {
  return DisplaySymbolName(type.ToString());
}

std::string Signature(const frontend::Function& function) {
  return DisplaySymbolName(FunctionSignature(function));
}

CompletionKind MethodKind(const frontend::Function& method) {
  return method.is_getter || method.is_setter ? CompletionKind::Property : CompletionKind::Method;
}

Completion StandardClassCompletion(const frontend::Class& declaration) {
  const auto& name = declaration.name;
  return {
      .label = name,
      .detail =
          name == "ByteArray" || name == "ByteData" || name == "RegExp" || name == "RegExpMatch"
              ? std::string(BuiltinTypeDetail(name))
              : name,
      .kind = CompletionKind::Type,
      .documentation = {},
  };
}

std::string MethodDetail(const frontend::Function& method) {
  if (method.is_getter) {
    return TypeText(method.return_type);
  }
  if (method.is_setter && !method.parameters.empty()) {
    return TypeText(method.parameters.front().type);
  }
  return Signature(method);
}

std::string GenericName(std::string_view source_name,
                        std::span<const frontend::TypeParameter> parameters) {
  auto name = DisplaySymbolName(source_name);
  if (!parameters.empty()) {
    name += '<';
    for (const auto& parameter : parameters) {
      name += (name.back() == '<' ? "" : ", ") + parameter.name;
      if (parameter.bound) {
        name += " extends " + TypeText(*parameter.bound);
      }
    }
    name += '>';
  }
  return name;
}

}  // namespace

template <class Declaration, class Detail>
std::size_t SemanticIndex::DeclareSourceSymbol(const Declaration& declaration,
                                               CompletionKind kind,
                                               Detail detail) {
  const auto existing = declarations_.find(KeyAt(declaration.location));
  const auto documentation = DocumentationLocation(declaration);
  return existing == declarations_.end()
             ? AddSymbol(declaration.location, kind, detail(), documentation)
             : existing->second;
}

template <class Declaration, class Detail>
std::size_t SemanticIndex::DeclareSymbol(const Declaration& declaration,
                                         CompletionKind kind,
                                         Detail detail) {
  const auto symbol_id = DeclareSourceSymbol(declaration, kind, detail);
  symbols_[symbol_id].export_kind = declaration.export_kind;
  names_.insert_or_assign(declaration.name, symbol_id);
  return symbol_id;
}

template <class Declaration, class Detail>
std::size_t SemanticIndex::DeclareMember(std::string_view owner,
                                         const Declaration& declaration,
                                         CompletionKind kind,
                                         Detail detail) {
  const auto symbol_id = DeclareSourceSymbol(declaration, kind, detail);
  auto& symbol = symbols_[symbol_id];
  if (symbol.owner.empty()) {
    symbol.owner = DisplaySymbolName(owner);
  }
  symbol.visibility = declaration.visibility;
  symbol.storage = declaration.storage;
  members_.try_emplace(owner).first->second.insert_or_assign(declaration.name, symbol_id);
  return symbol_id;
}

void SemanticIndex::CollectDeclarations() {
  for (const auto declarations :
       {std::span(program_.functions), std::span(program_.templates.functions)}) {
    for (const auto& declaration : declarations) {
      const auto symbol = DeclareSymbol(declaration, CompletionKind::Function,
                                        [&] { return Signature(declaration); });
      CollectParameters(symbol, declaration);
    }
  }
  for (const auto& declaration : program_.globals) {
    const auto checked = model_.globals.find(declaration.name);
    DeclareSymbol(declaration,
                  declaration.binding == frontend::BindingKind::Constant ? CompletionKind::Constant
                                                                         : CompletionKind::Variable,
                  [&] { return TypeText(declaration.type); });
    if (checked != model_.globals.end()) {
      symbols_[names_.at(declaration.name)].completion.detail =
          DisplaySymbolName(checked->second.type.name);
    }
  }
  for (const auto declarations :
       {std::span(program_.aliases), std::span(program_.templates.aliases)}) {
    for (const auto& declaration : declarations) {
      DeclareSymbol(declaration, CompletionKind::Type, [&] {
        return "type " + GenericName(declaration.name, declaration.type_parameters) + " = " +
               TypeText(declaration.target);
      });
    }
  }
  for (const auto& declaration : program_.classes) {
    CollectClass(declaration);
  }
  for (const auto& declaration : program_.templates.classes) {
    CollectClass(declaration);
  }
  for (const auto& declaration : program_.templates.class_applications) {
    CollectClass(*declaration);
  }
  for (const auto& declaration : program_.interfaces) {
    CollectInterface(declaration);
  }
  for (const auto& declaration : program_.templates.interfaces) {
    CollectInterface(declaration);
  }
  for (const auto& declaration : program_.templates.interface_applications) {
    CollectInterface(*declaration);
  }
  for (const auto& declaration : program_.enums) {
    DeclareSymbol(declaration, CompletionKind::Enum, [] { return "enum"; });
    CollectEnumMembers(declaration);
  }
  if (!model_.classes.empty()) {
    CollectConstructorParameters();
  }
}

void SemanticIndex::CollectConstructorParameters() {
  absl::flat_hash_set<std::string_view> collected;
  for (const auto& [name, owner] : model_.classes) {
    CollectConstructorParameters(owner, collected);
  }
}

void SemanticIndex::CollectConstructorParameters(const frontend::ClassDescriptor& owner,
                                                 absl::flat_hash_set<std::string_view>& collected) {
  if (!collected.insert(owner.type.name).second) {
    return;
  }
  const auto child = names_.find(owner.type.name);
  const auto& signature = owner.constructor;
  if (child == names_.end() || signature.parameters.empty()) {
    return;
  }
  const frontend::ClassDescriptor* base = nullptr;
  if (owner.base_class) {
    base = &model_.classes.at(*owner.base_class);
    CollectConstructorParameters(*base, collected);
  }
  // 基类先完成；本轮循环不再插入 owner，避免 parameters_ 扩容使借用失效。
  auto& parameters = parameters_[child->second];
  const auto members = members_.find(owner.type.name);
  const auto base_symbol = base != nullptr ? names_.find(base->type.name) : names_.end();
  const auto* base_parameters =
      base_symbol != names_.end() ? ParameterSymbols(base_symbol->second) : nullptr;
  const auto* fields = members != members_.end() ? &members->second : nullptr;
  for (std::size_t index = 0; index < owner.constructor.parameters.size(); ++index) {
    const auto& parameter = owner.constructor.parameters[index];
    const auto& binding = owner.constructor_bindings.at(index);
    const auto* source = binding.declaration;
    if (binding.field == nullptr && !binding.base_parameter) {
      continue;
    }
    std::optional<std::size_t> symbol;
    if (binding.base_parameter && base != nullptr) {
      symbol = FindNamedSymbol(base_parameters,
                               base->constructor.parameters.at(*binding.base_parameter).name);
    } else if (binding.field != nullptr) {
      symbol = FindNamedSymbol(fields, source != nullptr && !source->member_name.empty()
                                           ? source->member_name
                                           : parameter.name);
    }
    if (!symbol) {
      continue;
    }
    parameters.insert_or_assign(parameter.name, *symbol);
    if (source != nullptr && source->binding != frontend::Parameter::Binding::Local) {
      references_.insert_or_assign(KeyAt(source->location), Reference{.symbol = *symbol});
    }
  }
}

void SemanticIndex::CollectClass(const frontend::Class& declaration) {
  const auto kind = frontend::IsStandardTypeNamespace(program_, declaration)
                        ? CompletionKind::Type
                        : CompletionKind::Class;
  const auto* checked = model_.FindClassDescriptor(declaration);
  std::optional<std::string> detail;
  const auto class_detail = [&] -> std::string& {
    if (!detail) {
      detail = ClassCompletion(declaration, checked).detail;
    }
    return *detail;
  };
  const auto class_symbol = DeclareSymbol(declaration, kind, class_detail);
  if (checked != nullptr) {
    const auto origin = program_.origins.find(declaration.name);
    if (!declaration.type_parameters.empty() ||
        (!checked->type.is_abstract &&
         (origin == program_.origins.end() || origin->second.arguments.empty()))) {
      // 初次声明先于类型检查；工厂只创建新符号，此处更新检查后才得到的继承签名。
      symbols_[class_symbol].completion.detail = std::move(class_detail());
    }
  }
  NamedSymbols* field_parameters = nullptr;
  for (const auto& field : declaration.fields) {
    const auto field_symbol = DeclareMember(declaration.name, field, CompletionKind::Field,
                                            [&] { return TypeText(field.type); });
    if (declaration.IsConstructorField(field)) {
      if (field_parameters == nullptr) {
        field_parameters = &parameters_[class_symbol];
      }
      field_parameters->insert_or_assign(field.name, field_symbol);
    }
  }
  for (const auto methods :
       {std::span(declaration.methods), std::span(declaration.method_templates)}) {
    for (const auto& method : methods) {
      const auto method_symbol = DeclareMember(declaration.name, method, MethodKind(method),
                                               [&] { return MethodDetail(method); });
      BindIntrinsicSignature(method_symbol, declaration, method);
      if (method.intrinsic == Builtin::JsonTypedParse && !method.type_parameters.empty()) {
        const auto plain =
            std::ranges::find(declaration.methods, "parse", &frontend::Function::name);
        symbols_[method_symbol].completion.detail = Signature(*plain) + " | " + Signature(method);
      }
      CollectParameters(method_symbol, method);
    }
  }
  if (!declaration.constructor ||
      declaration.constructor_kind != frontend::ConstructorKind::Explicit) {
    return;
  }
  CollectParameters(class_symbol, *declaration.constructor,
                    checked != nullptr ? &checked->constructor : nullptr);
}

void SemanticIndex::CollectInterface(const frontend::Interface& declaration) {
  DeclareSymbol(declaration, CompletionKind::Interface, [&] {
    return "interface " + GenericName(declaration.name, declaration.type_parameters);
  });
  for (const auto methods :
       {std::span(declaration.methods), std::span(declaration.method_templates)}) {
    for (const auto& method : methods) {
      const auto method_symbol = DeclareMember(declaration.name, method, MethodKind(method),
                                               [&] { return MethodDetail(method); });
      CollectParameters(method_symbol, method);
    }
  }
}

void SemanticIndex::BindIntrinsicSignature(std::size_t symbol,
                                           const frontend::Class& owner,
                                           const frontend::Function& method) {
  if (!method.intrinsic || !method.type_parameters.empty()) {
    return;
  }
  if (const auto checked = model_.classes.find(owner.name); checked != model_.classes.end()) {
    symbols_[symbol].completion.detail = BuiltinDetail(checked->second.methods.at(method.name));
  }
}

void SemanticIndex::CollectEnumMembers(const frontend::Enum& declaration) {
  auto& members = members_.try_emplace(declaration.name).first->second;
  const auto owner = DisplaySymbolName(declaration.name);
  std::size_t ordinal = 0;
  for (const auto& value : declaration.members) {
    const auto symbol_id = AddSymbol(value.location, CompletionKind::EnumMember,
                                     owner + '.' + value.name + " = " + std::to_string(ordinal++));
    symbols_[symbol_id].owner = owner;
    symbols_[symbol_id].storage = frontend::MemberStorage::Static;
    members.insert_or_assign(value.name, symbol_id);
  }
}

void SemanticIndex::CollectParameters(std::size_t symbol,
                                      const frontend::Function& function,
                                      const frontend::FunctionDescriptor* checked) {
  NamedSymbols* declared = nullptr;
  for (const auto& parameter : function.parameters) {
    if (parameter.binding != frontend::Parameter::Binding::Local) {
      continue;
    }
    const auto detail = [&] {
      return checked != nullptr
                 ? DisplayType(
                       checked->parameters.at(checked->parameter_indices.at(parameter.name)).type)
                 : TypeText(parameter.type);
    };
    bool created = false;
    const auto parameter_id = DeclareSourceSymbol(parameter, CompletionKind::Variable, [&] {
      created = true;
      return detail();
    });
    auto& parameter_symbol = symbols_[parameter_id];
    if (checked != nullptr && !created) {
      parameter_symbol.completion.detail = detail();
    }
    if (declared == nullptr) {
      declared = &parameters_[symbol];
    }
    declared->try_emplace(parameter.name, parameter_id);
    parameter_symbol.role = Symbol::Role::Parameter;
  }
}

std::string SemanticIndex::DefaultValue(const Expression& expression) const {
  const auto& source = sources_[expression.location.source_id];
  const auto& token = source.tokens[TokenAt(expression.location)];
  switch (expression.kind) {
    case Expression::Kind::IntegerLiteral:
      return expression.text;
    case Expression::Kind::DoubleLiteral:
    case Expression::Kind::BooleanLiteral:
    case Expression::Kind::StringLiteral:
    case Expression::Kind::NullLiteral:
      return std::string(source.text.substr(token.offset, token.length));
    case Expression::Kind::Unary:
      return std::string(source.text.substr(token.offset, token.length)) +
             DefaultValue(*expression.operand);
    default:
      return "…";
  }
}

Completion SemanticIndex::ClassCompletion(const frontend::Class& declaration,
                                          const frontend::ClassDescriptor* checked) const {
  if (frontend::IsStandardTypeNamespace(program_, declaration)) {
    return StandardClassCompletion(declaration);
  }
  Completion result{
      .label = declaration.name,
      .detail = std::string(declaration.native_key.empty() ? "class " : "external class ") +
                GenericName(declaration.name, declaration.type_parameters),
      .kind = CompletionKind::Class,
      .documentation = {},
  };
  auto& text = result.detail;
  if (!declaration.native_key.empty()) {
    text += " = '" + declaration.native_key + '\'';
  }
  if (declaration.constructor_kind == frontend::ConstructorKind::Unavailable) {
    text += " {}";
    return result;
  }
  text += " {\n  ";
  text += ConstructorParameters(declaration, checked);
  text += "\n}";
  return result;
}

std::string SemanticIndex::ConstructorParameters(const frontend::Class& declaration,
                                                 const frontend::ClassDescriptor* checked) const {
  std::string text;
  if (declaration.constructor &&
      declaration.constructor->visibility == frontend::Visibility::Private) {
    text += frontend::kPrivateNamePrefix;
  }
  text += "constructor(";
  if (checked != nullptr) {
    const auto& constructor = checked->constructor;
    for (std::size_t index = 0; index < constructor.parameters.size(); ++index) {
      const auto& parameter = constructor.parameters[index];
      AppendConstructorParameter(text, parameter.name, DisplayType(parameter.type));
      if (constructor.optional_parameters.contains(parameter.name)) {
        const auto* initial = ConstructorDefault(*checked, index);
        text += " = ";
        text += initial != nullptr ? DefaultValue(*initial) : "…";
      }
    }
    return text + ");";
  }
  if (declaration.constructor_kind == frontend::ConstructorKind::Explicit) {
    for (const auto& parameter : declaration.constructor->parameters) {
      AppendConstructorParameter(text, parameter.name, TypeText(parameter.type));
    }
    return text + ");";
  }
  for (const auto& field : declaration.fields) {
    if (!declaration.IsConstructorField(field)) {
      continue;
    }
    AppendConstructorParameter(text, field.name, TypeText(field.type));
    if (field.initializer) {
      text += " = " + DefaultValue(*field.initializer);
    }
  }

  return text + ");";
}

}  // namespace vela::tooling
