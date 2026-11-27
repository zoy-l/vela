#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"

namespace vela::compiler::test {

inline std::optional<frontend::TypeName> ParseReflectionType(std::string_view source) {
  auto parsed = frontend::ParseTypeSource(source);
  if (!parsed) {
    ADD_FAILURE() << frontend::DiagnosticsText(parsed.error());
    return std::nullopt;
  }
  return std::move(*parsed);
}

inline frontend::ReflectionMemberSelection SelectMember(
    frontend::ReflectionMemberKind kind,
    std::string_view name = {},
    std::initializer_list<std::string_view> type_arguments = {}) {
  frontend::ReflectionMemberSelection member{
      .kind = kind,
      .name = std::string(name),
      .type_arguments = {},
      .location = {},
  };
  for (const auto argument : type_arguments) {
    auto parsed = ParseReflectionType(argument);
    if (!parsed) {
      return {};
    }
    member.type_arguments.push_back(std::move(*parsed));
  }
  return member;
}

inline frontend::ReflectionSelection SelectReflection(
    std::string_view type,
    std::vector<frontend::ReflectionMemberSelection> members,
    std::string_view module = "main.vela") {
  auto parsed = ParseReflectionType(type);
  if (!parsed) {
    return {};
  }
  return {
      .module = std::string(module),
      .type = std::move(*parsed),
      .members = std::move(members),
      .source_path = "<reflection>",
      .location = {},
      .source = frontend::ParseSourceSnapshot({}).source,
  };
}

inline constexpr std::string_view kDerivedCallArgumentsSource = R"(
class Argument extends CallArgument {
  extra: string = 'retained';
  constructor(value: dynamic) { super(value: value); }
}
class Arguments extends CallArguments {
  extra: int = 1;
  constructor(items: CallArgument[]) { super(items: items); }
}
)";

}  // namespace vela::compiler::test
