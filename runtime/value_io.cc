#include "runtime/value_io.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <memory>
#include <ostream>
#include <span>
#include <string_view>

#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {

auto FormatScalar(Value value, std::span<char, kScalarTextCapacity> buffer) -> std::string_view {
  if (value.IsBoolean()) {
    return value.AsBoolean() ? "true" : "false";
  }
  const auto converted =
      value.IsInteger()
          ? std::to_chars(buffer.data(), std::to_address(buffer.end()), value.AsInteger())
          : std::to_chars(buffer.data(), std::to_address(buffer.end()), value.AsDouble());
  return {buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())};
}

void WriteValue(std::ostream& output, Value value) {
  if (value.IsInteger() || value.IsDouble() || value.IsBoolean()) {
    std::array<char, kScalarTextCapacity> buffer{};
    output << FormatScalar(value, buffer);
  } else if (value.IsString()) {
    output << value.AsString()->value();
  } else if (value.IsNull()) {
    output << "null";
  } else {
    output << "object";
  }
}

}  // namespace vela::runtime
