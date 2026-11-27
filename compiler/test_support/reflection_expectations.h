#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>

#include <vela/error.h>

namespace vela::compiler::test {

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
