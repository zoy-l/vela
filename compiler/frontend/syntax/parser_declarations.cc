#include "compiler/frontend/syntax/names.h"
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

std::optional<std::vector<Attribute>> Parser::ParseAttributes() {
  std::vector<Attribute> attributes;
  while (Match(TokenKind::LeftBracket)) {
    AnnotatePrevious(TokenRole::AttributeDelimiter);
    const auto location = Previous().location;
    auto type = ParseType();
    if (!type) {
      return std::nullopt;
    }
    Attribute attribute{.type = std::move(*type), .arguments = {}, .location = location};
    if (Match(TokenKind::LeftParen)) {
      attribute.arguments = ParseArguments();
      for (const auto& argument : attribute.arguments) {
        if (argument.name.empty()) {
          AddDiagnostic(argument.location, "attribute arguments must be named");
          return std::nullopt;
        }
      }
      if (Expect(TokenKind::RightParen, "expected ')' after attribute arguments") == nullptr) {
        return std::nullopt;
      }
    }
    if (Expect(TokenKind::RightBracket, "expected ']' after attribute") == nullptr) {
      return std::nullopt;
    }
    AnnotatePrevious(TokenRole::AttributeDelimiter);
    attributes.push_back(std::move(attribute));
  }
  return attributes;
}

std::optional<Enum> Parser::ParseEnum() {
  const auto* name_token = Expect(TokenKind::Identifier, "expected enum name");
  if (name_token == nullptr) {
    return std::nullopt;
  }
  if (Expect(TokenKind::LeftBrace, "expected '{' after enum name") == nullptr) {
    return std::nullopt;
  }

  Enum declaration;
  declaration.name = name_token->lexeme;
  declaration.location = name_token->location;
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    auto attributes = ParseAttributes();
    if (!attributes) {
      return std::nullopt;
    }
    const auto* member = Expect(TokenKind::Identifier, "expected enum member name");
    if (member == nullptr) {
      return std::nullopt;
    }
    declaration.members.push_back({
        .name = member->lexeme,
        .location = member->location,
        .attributes = std::move(*attributes),
    });
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (Expect(TokenKind::RightBrace, "expected '}' after enum members") == nullptr) {
    return std::nullopt;
  }
  return declaration;
}

std::optional<Class> Parser::ParseClassHeader(ClassKind kind) {
  const auto* const name = Expect(TokenKind::Identifier, "expected class name");
  if (name == nullptr) {
    return std::nullopt;
  }
  Class declaration;
  declaration.name = name->lexeme;
  declaration.location = name->location;
  if (kind == ClassKind::Native) {
    if (Expect(TokenKind::Equal, "expected '=' before native class key") == nullptr) {
      return std::nullopt;
    }
    const auto* const key = Expect(TokenKind::StringLiteral, "expected native class key string");
    if (key == nullptr) {
      return std::nullopt;
    }
    if (!IsNativeTypeKey(key->lexeme)) {
      AddDiagnostic(name->location,
                    "native class key requires dot-separated non-keyword identifiers");
      return std::nullopt;
    }
    declaration.native_key = key->lexeme;
    declaration.constructor_kind = ConstructorKind::Unavailable;
  }
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
  if (kind == ClassKind::Native && (!declaration.type_parameters.empty() ||
                                    declaration.base_class || !declaration.interfaces.empty())) {
    AddDiagnostic(declaration.location,
                  "external classes cannot declare generics, bases or interfaces");
    return std::nullopt;
  }
  return declaration;
}

std::optional<Class> Parser::ParseClass(ClassKind kind) {
  auto header = ParseClassHeader(kind);
  if (!header) {
    return std::nullopt;
  }
  auto declaration = std::move(*header);
  if (Expect(TokenKind::LeftBrace, "expected '{' after class name") == nullptr) {
    return std::nullopt;
  }
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    if (ParseClassMember(declaration) == MemberParseResult::Failed) {
      return std::nullopt;
    }
  }
  if (Expect(TokenKind::RightBrace, "expected '}' after class body") == nullptr) {
    return std::nullopt;
  }
  return declaration;
}

Parser::MemberModifiers Parser::ParseMemberModifiers() {
  MemberModifiers modifiers;
  bool has_visibility = false;
  bool has_storage = false;
  const auto is_accessor_modifier = [&]() {
    return (Check(TokenKind::Get) || Check(TokenKind::Set)) &&
           IsMemberName(tokens_[current_ + 1].kind);
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

std::unique_ptr<Function> Parser::CreateConstructor(SourceLocation location) {
  auto constructor = std::make_unique<Function>();
  constructor->name = kConstructorName;
  constructor->location = location;
  constructor->return_type = {
      .name = std::string(kVoidTypeName),
      .location = location,
      .kind = TypeName::Kind::Named,
      .arguments = {},
      .parameter = {},
  };
  return constructor;
}

std::unique_ptr<Function> Parser::ParseConstructor(Class& declaration, MemberModifiers modifiers) {
  const auto visibility = modifiers.visibility;
  const auto storage = modifiers.storage;
  if (declaration.constructor || storage == MemberStorage::Static) {
    AddDiagnostic(Previous().location, "invalid or duplicate constructor declaration");
    return nullptr;
  }
  auto constructor = CreateConstructor(Previous().location);
  constructor->visibility = visibility;
  if (Match(TokenKind::LeftParen)) {
    declaration.constructor_kind = ConstructorKind::Explicit;
    auto parameters = ParseParameters();
    if (!parameters) {
      return nullptr;
    }
    constructor->parameters = std::move(*parameters);
    if (Expect(TokenKind::RightParen, "expected ')' after constructor parameters") == nullptr) {
      return nullptr;
    }
  }
  constructor->body = ParseBlock();
  return constructor->body ? std::move(constructor) : nullptr;
}

Parser::MemberParseResult Parser::ParseClassMember(Class& declaration) {
  auto attributes = ParseAttributes();
  if (!attributes) {
    return MemberParseResult::Failed;
  }
  if (!declaration.native_key.empty()) {
    return ParseNativeClassMember(declaration, std::move(*attributes));
  }
  const auto modifiers = ParseMemberModifiers();
  const bool is_async = Match(TokenKind::Async);
  const auto append_method = [&declaration, &attributes, modifiers](Function method) -> void {
    method.attributes = std::move(*attributes);
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
    constructor->attributes = std::move(*attributes);
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
  field->attributes = std::move(*attributes);
  declaration.fields.push_back(std::move(*field));
  return MemberParseResult::Parsed;
}

std::optional<Field> Parser::ParseField(const Token& name, MemberModifiers modifiers) {
  if (Expect(TokenKind::Colon, "expected ':' after field name") == nullptr) {
    return std::nullopt;
  }
  auto type = ParseType();
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
  if (Expect(TokenKind::Semicolon, "expected ';' after field declaration") == nullptr) {
    return std::nullopt;
  }
  return Field{
      .type = std::move(*type),
      .name = name.lexeme,
      .location = name.location,
      .visibility = modifiers.visibility,
      .storage = modifiers.storage,
      .initializer = std::move(initializer),
      .attributes = {},
  };
}

std::optional<Interface> Parser::ParseInterface() {
  const auto* const name = Expect(TokenKind::Identifier, "expected interface name");
  if (name == nullptr) {
    return std::nullopt;
  }
  Interface declaration;
  declaration.name = name->lexeme;
  declaration.location = name->location;
  auto type_parameters = ParseTypeParameters();
  if (!type_parameters ||
      (Expect(TokenKind::LeftBrace, "expected '{' after interface name") == nullptr)) {
    return std::nullopt;
  }
  declaration.type_parameters = std::move(*type_parameters);
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    auto attributes = ParseAttributes();
    if (!attributes) {
      return std::nullopt;
    }
    auto method = ParseInterfaceMethod();
    if (!method) {
      return std::nullopt;
    }
    method->attributes = std::move(*attributes);
    declaration.methods.push_back(std::move(*method));
  }
  if (Expect(TokenKind::RightBrace, "expected '}' after interface body") == nullptr) {
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
  auto signature = ParseSignature(std::move(method), SignatureContext::Interface);
  if (!signature ||
      Expect(TokenKind::Semicolon, "expected ';' after interface method") == nullptr) {
    return std::nullopt;
  }
  return signature;
}

std::optional<Function> Parser::ParseSignature(Function function, SignatureContext context) {
  const bool interface = context == SignatureContext::Interface;
  auto type_parameters = ParseTypeParameters();
  if (!type_parameters ||
      Expect(TokenKind::LeftParen, interface ? "expected '(' after interface method name"
                                             : "expected '(' after function name") == nullptr) {
    return std::nullopt;
  }
  function.type_parameters = std::move(*type_parameters);
  if (function.is_async && !function.native_symbol.empty()) {
    AddDiagnostic(function.location, "external functions cannot be async");
    return std::nullopt;
  }
  if (!function.native_symbol.empty() && !function.type_parameters.empty() &&
      context_ != SyntaxContext::Standard) {
    AddDiagnostic(function.location, "external functions cannot declare generic parameters");
    return std::nullopt;
  }
  auto parameters = ParseParameters();
  if (!parameters) {
    return std::nullopt;
  }
  function.parameters = std::move(*parameters);
  if (Expect(TokenKind::RightParen, interface ? "expected ')' after interface parameters"
                                              : "expected ')' after parameters") == nullptr ||
      Expect(TokenKind::Colon, interface ? "expected ':' before interface return type"
                                         : "expected ':' before return type") == nullptr) {
    return std::nullopt;
  }
  auto returned = ParseType();
  if (!returned) {
    return std::nullopt;
  }
  function.return_type = std::move(*returned);
  return function;
}

std::optional<Function> Parser::ParseFunction() {
  const bool is_async = Match(TokenKind::Async);
  const bool external = Match(TokenKind::External);
  if (Expect(TokenKind::Func, "expected 'func' before function declaration") == nullptr) {
    return std::nullopt;
  }
  const auto* const name = TakeName();
  if (name == nullptr) {
    AddDiagnostic(Peek().location, "expected function name");
    return std::nullopt;
  }
  return ParseFunctionBody(*name, external ? name->lexeme : std::string(), is_async);
}

std::optional<Function> Parser::ParseFunctionBody(const Token& name,
                                                  std::string native_symbol,
                                                  bool is_async) {
  Function declaration;
  declaration.name = name.lexeme;
  declaration.location = name.location;
  declaration.native_symbol = std::move(native_symbol);
  declaration.is_async = is_async;
  auto signature = ParseSignature(std::move(declaration), SignatureContext::Function);
  if (!signature) {
    return std::nullopt;
  }
  auto& function = *signature;
  if (context_ == SyntaxContext::Standard && Match(TokenKind::Semicolon)) {
    return std::move(function);
  }
  if (!function.native_symbol.empty()) {
    if (Match(TokenKind::Equal)) {
      const auto* const symbol = Expect(TokenKind::StringLiteral, "expected native symbol string");
      if (symbol == nullptr) {
        return std::nullopt;
      }
      if (symbol->lexeme.empty() || symbol->lexeme.contains('\0')) {
        AddDiagnostic(name.location, "native symbol must be nonempty and contain no null byte");
        return std::nullopt;
      }
      function.native_symbol = symbol->lexeme;
    }
    return (Expect(TokenKind::Semicolon, "expected ';' after external function") != nullptr)
               ? std::optional<Function>(std::move(function))
               : std::nullopt;
  }
  function.body = ParseBlock();
  return function.body ? std::optional<Function>(std::move(function)) : std::nullopt;
}

std::optional<std::vector<Parameter>> Parser::ParseParameters(ParameterTyping typing) {
  std::vector<Parameter> parameters;
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    auto parameter = ParseParameter(typing);
    if (!parameter) {
      return std::nullopt;
    }
    parameters.push_back(std::move(*parameter));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  return parameters;
}

std::optional<Parameter> Parser::ParseParameter(ParameterTyping typing) {
  const auto* const name = TakeName();
  if (name == nullptr) {
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
  auto type = ParseType();
  if (!type) {
    return std::nullopt;
  }
  return Parameter{.type = std::move(*type), .name = name->lexeme, .location = name->location};
}

}  // namespace vela::frontend
