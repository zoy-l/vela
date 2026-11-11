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
