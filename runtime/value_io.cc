#include "runtime/value_io.h"

#include <array>
#include <charconv>
#include <memory>
#include <ostream>
#include <string_view>

#include "runtime/string.h"

namespace vela::runtime {

void WriteValue(std::ostream& output, Value value) {
  if (value.IsInteger()) {
    output << value.AsInteger();
  } else if (value.IsDouble()) {
    // binary64 的最短往返文本含符号、17 位有效数字及十进制指数，32 字节足够。
    constexpr std::size_t kDoubleTextCapacity = 32;
    std::array<char, kDoubleTextCapacity> buffer{};
    const auto converted =
        std::to_chars(buffer.data(), std::to_address(buffer.end()), value.AsDouble());
    output << std::string_view(buffer.data(),
                               static_cast<std::size_t>(converted.ptr - buffer.data()));
  } else if (value.IsBoolean()) {
    output << (value.AsBoolean() ? "true" : "false");
  } else if (value.IsString()) {
    output << value.AsString()->value;
  } else if (value.IsNull()) {
    output << "null";
  } else {
    output << "object";
  }
}

}  // namespace vela::runtime
