#include "compiler/frontend/syntax/parser.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

std::optional<Enum> Parser::ParseEnum() {
  const Token name = Peek();
  const std::optional<Token> name_token = Expect(TokenKind::Identifier, "expected enum name");
  if (!name_token.has_value()) {
    return std::nullopt;
  }
  if (!Expect(TokenKind::LeftBrace, "expected '{' after enum name").has_value()) {
    return std::nullopt;
  }

  Enum declaration;
  declaration.name = name_token->lexeme;
  declaration.location = name.location;
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    const std::optional<Token> member = Expect(TokenKind::Identifier, "expected enum member name");
    if (!member.has_value()) {
      return std::nullopt;
    }
    declaration.members.push_back({.name = member->lexeme, .location = member->location});
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightBrace, "expected '}' after enum members").has_value()) {
    return std::nullopt;
  }
  return declaration;
}

std::optional<Class> Parser::ParseClass() {
  const auto name = Expect(TokenKind::Identifier, "expected class name");
  if (!name) {
    return std::nullopt;
  }
  Class declaration;
  declaration.name = name->lexeme;
  declaration.location = name->location;
  auto type_parameters = ParseTypeParameters();
  if (!type_parameters) {
    return std::nullopt;
  }
  declaration.type_parameters = std::move(*type_parameters);
  if (Match(TokenKind::Extends)) {
    auto base = ParseType();
    if (!base) {
      return std::nullopt;
    }
    declaration.base_class = std::move(*base);
  }
  if (Match(TokenKind::Implements)) {
    while (true) {
      auto interface = ParseType();
      if (!interface) {
        return std::nullopt;
      }
      declaration.interfaces.push_back(std::move(*interface));
      if (!Match(TokenKind::Comma)) {
        break;
      }
    }
  }
  if (!Expect(TokenKind::LeftBrace, "expected '{' after class name")) {
    return std::nullopt;
  }
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    if (ParseClassMember(declaration) == MemberParseResult::Failed) {
      return std::nullopt;
    }
  }
  if (!Expect(TokenKind::RightBrace, "expected '}' after class body")) {
    return std::nullopt;
  }
  return declaration;
}

Parser::MemberModifiers Parser::ParseMemberModifiers() {
  MemberModifiers modifiers;
  bool has_visibility = false;
  bool has_storage = false;
  const auto is_accessor_modifier = [&]() {
    return (Check(TokenKind::Get) || Check(TokenKind::Set)) && current_ + 1 < tokens_.size() &&
           IsMemberName(tokens_.subspan(current_ + 1).front().kind);
  };
  while (Check(TokenKind::Private) || Check(TokenKind::Static) || Check(TokenKind::Override) ||
         is_accessor_modifier()) {
    const auto modifier = Advance();
    if (modifier.kind == TokenKind::Private) {
      if (has_visibility) {
        AddDiagnostic(modifier.location, "duplicate visibility modifier");
      }
      has_visibility = true;
      modifiers.visibility = Visibility::Private;
    } else if (modifier.kind == TokenKind::Static) {
      if (has_storage) {
        AddDiagnostic(modifier.location, "duplicate static modifier");
      }
      has_storage = true;
      modifiers.storage = MemberStorage::Static;
    } else if (modifier.kind == TokenKind::Override) {
      modifiers.is_override = true;
    } else if (modifier.kind == TokenKind::Get) {
      modifiers.is_getter = true;
    } else if (modifier.kind == TokenKind::Set) {
      modifiers.is_setter = true;
    }
  }
  return modifiers;
}

std::unique_ptr<Function> Parser::ParseConstructor(Class& declaration, MemberModifiers modifiers) {
  const auto visibility = modifiers.visibility;
  const auto storage = modifiers.storage;
  if (declaration.constructor || storage == MemberStorage::Static) {
    AddDiagnostic(Previous().location, "invalid or duplicate constructor declaration");
    return nullptr;
  }
  auto constructor = std::make_unique<Function>();
  constructor->name = "$constructor";
  constructor->location = Previous().location;
  constructor->visibility = visibility;
  constructor->return_type.name = kVoidTypeName;
  constructor->return_type.location = constructor->location;
  if (Match(TokenKind::LeftParen)) {
    declaration.constructor_kind = ConstructorKind::Explicit;
    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
      auto parameter = ParseParameter();
      if (!parameter) {
        return nullptr;
      }
      constructor->parameters.push_back(std::move(*parameter));
      if (!Match(TokenKind::Comma)) {
        break;
      }
    }
    if (!Expect(TokenKind::RightParen, "expected ')' after constructor parameters")) {
      return nullptr;
    }
  }
  constructor->body = ParseBlock();
  return constructor->body ? std::move(constructor) : nullptr;
}

Parser::MemberParseResult Parser::ParseClassMember(Class& declaration) {
  const auto modifiers = ParseMemberModifiers();
  const bool is_async = Match(TokenKind::Async);
  const auto append_method = [&declaration, modifiers](Function method) -> void {
    method.visibility = modifiers.visibility;
    method.storage = modifiers.storage;
    method.is_override = modifiers.is_override;
    method.is_getter = modifiers.is_getter;
    method.is_setter = modifiers.is_setter;
    declaration.methods.push_back(std::move(method));
  };
  if (Check(TokenKind::External)) {
    if (is_async) {
      AddDiagnostic(Peek().location, "external methods cannot be async");
    }
    auto method = ParseFunction();
    if (!method) {
      return MemberParseResult::Failed;
    }
    if (modifiers.storage != MemberStorage::Static) {
      AddDiagnostic(method->location, "external methods must be static");
      return MemberParseResult::Failed;
    }
    append_method(std::move(*method));
    return MemberParseResult::Parsed;
  }
  if (Match(TokenKind::Constructor)) {
    auto constructor = ParseConstructor(declaration, modifiers);
    if (!constructor) {
      return MemberParseResult::Failed;
    }
    declaration.constructor = std::move(constructor);
    return MemberParseResult::Parsed;
  }
  std::optional<Token> member;
  if (IsMemberName(Peek().kind)) {
    member = Advance();
  } else {
    AddDiagnostic(Peek().location, "expected field or method name");
  }
  if (!member) {
    return MemberParseResult::Failed;
  }
  if (Check(TokenKind::LeftParen) || Check(TokenKind::Less)) {
    auto method = ParseFunctionBody(*member, {}, is_async);
    if (!method) {
      return MemberParseResult::Failed;
    }
    if ((modifiers.is_getter || modifiers.is_setter) && !method->type_parameters.empty()) {
      AddDiagnostic(method->location, "accessors cannot declare generic parameters");
      return MemberParseResult::Failed;
    }
    append_method(std::move(*method));
    return MemberParseResult::Parsed;
  }
  auto field = ParseField(*member, modifiers);
  if (!field) {
    return MemberParseResult::Failed;
  }
  declaration.fields.push_back(std::move(*field));
  return MemberParseResult::Parsed;
}

std::optional<Field> Parser::ParseField(const Token& name, MemberModifiers modifiers) {
  if (!Expect(TokenKind::Colon, "expected ':' after field name")) {
    return std::nullopt;
  }
  const auto type = ParseType();
  if (!type) {
    return std::nullopt;
  }
  std::unique_ptr<Expression> initializer;
  if (Match(TokenKind::Equal)) {
    initializer = ParseExpression();
    if (!initializer) {
      return std::nullopt;
    }
  }
  if (!Expect(TokenKind::Semicolon, "expected ';' after field declaration")) {
    return std::nullopt;
  }
  return Field{
      .type = *type,
      .name = name.lexeme,
      .location = name.location,
      .visibility = modifiers.visibility,
      .storage = modifiers.storage,
      .initializer = std::move(initializer),
  };
}

std::optional<Interface> Parser::ParseInterface() {
  const auto name = Expect(TokenKind::Identifier, "expected interface name");
  if (!name) {
    return std::nullopt;
  }
  Interface declaration;
  declaration.name = name->lexeme;
  declaration.location = name->location;
  auto type_parameters = ParseTypeParameters();
  if (!type_parameters || !Expect(TokenKind::LeftBrace, "expected '{' after interface name")) {
    return std::nullopt;
  }
  declaration.type_parameters = std::move(*type_parameters);
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    auto method = ParseInterfaceMethod();
    if (!method) {
      return std::nullopt;
    }
    declaration.methods.push_back(std::move(*method));
  }
  if (!Expect(TokenKind::RightBrace, "expected '}' after interface body")) {
    return std::nullopt;
  }
  return declaration;
}

std::optional<Function> Parser::ParseInterfaceMethod() {
  const auto member = Peek();
  if (!IsMemberName(member.kind)) {
    AddDiagnostic(Peek().location, "expected interface method name");
    return std::nullopt;
  }
  Advance();
  Function method;
  method.name = member.lexeme;
  method.location = member.location;
  auto type_parameters = ParseTypeParameters();
  if (!type_parameters ||
      !Expect(TokenKind::LeftParen, "expected '(' after interface method name")) {
    return std::nullopt;
  }
  method.type_parameters = std::move(*type_parameters);
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    auto parameter = ParseParameter();
    if (!parameter) {
      return std::nullopt;
    }
    method.parameters.push_back(std::move(*parameter));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightParen, "expected ')' after interface parameters") ||
      !Expect(TokenKind::Colon, "expected ':' before interface return type")) {
    return std::nullopt;
  }
  auto returned = ParseType();
  if (!returned || !Expect(TokenKind::Semicolon, "expected ';' after interface method")) {
    return std::nullopt;
  }
  method.return_type = std::move(*returned);
  return method;
}

std::optional<Function> Parser::ParseFunction() {
  const bool is_async = Match(TokenKind::Async);
  const bool external = Match(TokenKind::External);
  if (!Expect(TokenKind::Func, "expected 'func' before function declaration")) {
    return std::nullopt;
  }
  const auto name = TakeName();
  if (!name) {
    AddDiagnostic(Peek().location, "expected function name");
    return std::nullopt;
  }
  return ParseFunctionBody(*name, external ? name->lexeme : std::string(), is_async);
}

std::optional<Function> Parser::ParseFunctionBody(const Token& name,
                                                  std::string native_symbol,
                                                  bool is_async) {
  auto type_parameters = ParseTypeParameters();
  if (!type_parameters || !Expect(TokenKind::LeftParen, "expected '(' after function name")) {
    return std::nullopt;
  }
  Function function;
  function.name = name.lexeme;
  function.location = name.location;
  function.type_parameters = std::move(*type_parameters);
  function.native_symbol = std::move(native_symbol);
  function.is_async = is_async;
  if (function.is_async && !function.native_symbol.empty()) {
    AddDiagnostic(name.location, "external functions cannot be async");
    return std::nullopt;
  }
  if (!function.native_symbol.empty() && !function.type_parameters.empty()) {
    AddDiagnostic(name.location, "external functions cannot declare generic parameters");
    return std::nullopt;
  }
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    const auto parameter = ParseParameter();
    if (!parameter) {
      return std::nullopt;
    }
    function.parameters.push_back(*parameter);
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightParen, "expected ')' after parameters") ||
      !Expect(TokenKind::Colon, "expected ':' before return type")) {
    return std::nullopt;
  }
  const auto type = ParseType();
  if (!type) {
    return std::nullopt;
  }
  function.return_type = *type;
  if (!function.native_symbol.empty()) {
    if (Match(TokenKind::Equal)) {
      const auto symbol = Expect(TokenKind::StringLiteral, "expected native symbol string");
      if (!symbol || symbol->lexeme.empty() || symbol->lexeme.contains('\0')) {
        AddDiagnostic(name.location, "native symbol must be nonempty and contain no null byte");
        return std::nullopt;
      }
      function.native_symbol = symbol->lexeme;
    }
    return Expect(TokenKind::Semicolon, "expected ';' after external function")
               ? std::optional<Function>(std::move(function))
               : std::nullopt;
  }
  function.body = ParseBlock();
  return function.body ? std::optional<Function>(std::move(function)) : std::nullopt;
}

std::optional<Parameter> Parser::ParseParameter(ParameterTyping typing) {
  const auto name = TakeName();
  if (!name) {
    AddDiagnostic(Peek().location, "expected parameter name");
    return std::nullopt;
  }
  if (!Match(TokenKind::Colon)) {
    if (typing == ParameterTyping::Required) {
      AddDiagnostic(Peek().location, "expected ':' after parameter name");
      return std::nullopt;
    }
    return Parameter{
        .type = {.name = {}, .location = name->location, .arguments = {}, .parameter = {}},
        .name = name->lexeme,
        .location = name->location,
    };
  }
  const auto type = ParseType();
  if (!type) {
    return std::nullopt;
  }
  return Parameter{.type = *type, .name = name->lexeme, .location = name->location};
}

}  // namespace vela::frontend
