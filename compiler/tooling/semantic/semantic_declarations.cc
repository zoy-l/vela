#include "compiler/tooling/semantic/semantic_index.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "base/builtin.h"

#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/tooling/documentation/builtin_documentation.h"
#include "compiler/tooling/documentation/documentation.h"
#include "compiler/tooling/semantic/semantic_syntax.h"

namespace vela::tooling {
namespace {
using frontend::Expression;
using frontend::TypeName;

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
}

void SemanticIndex::CollectClass(const frontend::Class& declaration) {
  const auto kind = frontend::IsStandardTypeNamespace(program_, declaration)
                        ? CompletionKind::Type
                        : CompletionKind::Class;
  const auto class_symbol =
      DeclareSymbol(declaration, kind, [&] { return ClassCompletion(declaration).detail; });
  for (const auto& field : declaration.fields) {
    const auto field_symbol = DeclareMember(declaration.name, field, CompletionKind::Field,
                                            [&] { return TypeText(field.type); });
    if (declaration.IsConstructorField(field)) {
      parameters_.try_emplace(class_symbol)
          .first->second.insert_or_assign(field.name, field_symbol);
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
  if (declaration.constructor &&
      declaration.constructor_kind == frontend::ConstructorKind::Explicit) {
    CollectParameters(class_symbol, *declaration.constructor);
  }
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
    symbols_[symbol_id].storage = frontend::MemberStorage::Static;
    members.insert_or_assign(value.name, symbol_id);
  }
}

void SemanticIndex::CollectParameters(std::size_t symbol, const frontend::Function& function) {
  auto& declared = parameters_.try_emplace(symbol).first->second;
  for (const auto& parameter : function.parameters) {
    const auto parameter_id =
        AddSymbol(parameter.location, CompletionKind::Variable, TypeText(parameter.type));
    declared.insert_or_assign(parameter.name, parameter_id);
    symbols_[parameter_id].role = Symbol::Role::Parameter;
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

Completion SemanticIndex::ClassCompletion(const frontend::Class& declaration) const {
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
  if (declaration.constructor &&
      declaration.constructor->visibility == frontend::Visibility::Private) {
    text += "private ";
  }
  text += "constructor(";
  if (declaration.constructor_kind == frontend::ConstructorKind::Explicit) {
    for (const auto& parameter : declaration.constructor->parameters) {
      text += (text.back() == '(' ? "" : ", ") + parameter.name + ": " + TypeText(parameter.type);
    }
  } else {
    for (const auto& field : declaration.fields) {
      if (!declaration.IsConstructorField(field)) {
        continue;
      }
      text += (text.back() == '(' ? "" : ", ") + field.name + ": " + TypeText(field.type);
      if (field.initializer) {
        text += " = " + DefaultValue(*field.initializer);
      }
    }
  }
  text += ");\n}";
  return result;
}

}  // namespace vela::tooling
