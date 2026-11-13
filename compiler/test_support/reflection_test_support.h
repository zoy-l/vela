#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <vela/error.h>

namespace vela::compiler::test {

struct ReflectionInput final {
  std::string_view source;
  std::string_view configuration;
};

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
