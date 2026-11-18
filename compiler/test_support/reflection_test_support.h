#pragma once

#include <expected>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vela/error.h>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"

namespace vela::compiler::test {

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
    member.type_arguments.push_back(*frontend::ParseTypeSource(argument));
  }
  return member;
}

inline frontend::ReflectionSelection SelectReflection(
    std::string_view type,
    std::vector<frontend::ReflectionMemberSelection> members,
    std::string_view module = "main.vela") {
  return {
      .module = std::string(module),
      .type = *frontend::ParseTypeSource(type),
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

template <class T>
std::expected<T, std::string> RequireReflectionValue(Result<std::optional<T>> result) {
  if (!result) {
    return std::unexpected(result.error().message);
  }
  auto& value = *result;
  if (!value) {
    return std::unexpected("expected reflection result is missing");
  }
  return std::move(*value);
}

}  // namespace vela::compiler::test
