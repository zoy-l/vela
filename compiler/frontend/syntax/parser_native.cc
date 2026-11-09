#include "compiler/frontend/syntax/parser.h"

#include <memory>
#include <string>
#include <utility>

#include "base/native_binding.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

Parser::MemberParseResult Parser::ParseNativeConstructor(Class& declaration) {
  const auto location = Previous().location;
  if (declaration.constructor) {
    AddDiagnostic(location, "external class may declare only one constructor");
    return MemberParseResult::Failed;
  }
  if (Expect(TokenKind::LeftParen, "expected '(' after constructor") == nullptr) {
    return MemberParseResult::Failed;
  }
  auto parameters = ParseParameters();
  if (!parameters ||
      (Expect(TokenKind::RightParen, "expected ')' after constructor parameters") == nullptr) ||
      (Expect(TokenKind::Semicolon, "expected ';' after external constructor") == nullptr)) {
    return MemberParseResult::Failed;
  }
  auto constructor = std::make_unique<Function>();
  constructor->name = kConstructorName;
  constructor->location = location;
  constructor->parameters = std::move(*parameters);
  constructor->return_type = {
      .name = std::string(kVoidTypeName),
      .location = location,
      .kind = TypeName::Kind::Named,
      .arguments = {},
      .parameter = {},
  };
  constructor->storage = MemberStorage::Static;
  constructor->native_symbol =
      NativeMemberKey(declaration.native_key, NativeMemberKind::Constructor, {});
  declaration.constructor = std::move(constructor);
  declaration.constructor_kind = ConstructorKind::Explicit;
  return MemberParseResult::Parsed;
}

Parser::MemberParseResult Parser::ParseNativeClassMember(Class& declaration) {
  const auto modifiers = ParseMemberModifiers();
  if (modifiers.visibility != Visibility::Public || modifiers.is_override ||
      (modifiers.is_getter && modifiers.is_setter)) {
    AddDiagnostic(Peek().location, "external members must be public non-override declarations");
    return MemberParseResult::Failed;
  }
  if (Match(TokenKind::Constructor)) {
    if (modifiers.storage == MemberStorage::Static || modifiers.is_getter || modifiers.is_setter) {
      AddDiagnostic(Previous().location, "external constructor cannot have member modifiers");
      return MemberParseResult::Failed;
    }
    return ParseNativeConstructor(declaration);
  }
  auto method = ParseInterfaceMethod();
  if (!method) {
    return MemberParseResult::Failed;
  }
  if (!method->type_parameters.empty()) {
    AddDiagnostic(method->location, "external methods cannot declare generic parameters");
    return MemberParseResult::Failed;
  }
  method->storage = modifiers.storage;
  method->is_getter = modifiers.is_getter;
  method->is_setter = modifiers.is_setter;
  auto kind = method->storage == MemberStorage::Static ? NativeMemberKind::Static
                                                       : NativeMemberKind::Method;
  if (method->is_getter || method->is_setter) {
    if (method->storage == MemberStorage::Static) {
      AddDiagnostic(method->location, "external properties require an instance receiver");
      return MemberParseResult::Failed;
    }
    kind = method->is_getter ? NativeMemberKind::Getter : NativeMemberKind::Setter;
  }
  method->native_symbol = NativeMemberKey(declaration.native_key, kind, method->name);
  declaration.methods.push_back(std::move(*method));
  return MemberParseResult::Parsed;
}

}  // namespace vela::frontend
