#include "runtime/strings/string.h"

#include <bit>
#include <cstddef>
#include <cstdint>

#include "base/utf8.h"
#include "runtime/value/object.h"

namespace vela::runtime {

std::size_t StringObject::ByteOffset(std::uint32_t index) const noexcept {
  const auto bytes = value();
  const auto count = length();
  if (count == bytes.size()) {
    return index;
  }
  auto position = std::bit_cast<HeapObject::StringPosition>(header_.type_metadata);
  const auto distance = [](std::uint32_t first, std::uint32_t second) {
    return first > second ? first - second : second - first;
  };
  // 从上次位置或两端中较近的位置开始；尾部访问也不必先扫描整串。
  if (index < distance(index, position.index)) {
    position = {.index = 0, .offset = 0};
  }
  if (count - index < distance(index, position.index)) {
    position = {.index = count, .offset = static_cast<std::uint32_t>(bytes.size())};
  }
  while (position.index < index) {
    position.offset +=
        static_cast<std::uint32_t>(DecodeUtf8Prefix(bytes.substr(position.offset)).width);
    ++position.index;
  }
  while (position.index > index) {
    --position.offset;
    while ((static_cast<unsigned char>(bytes.substr(position.offset).front()) &
            kContinuationMask) == kContinuationTag) {
      --position.offset;
    }
    --position.index;
  }
  header_.type_metadata = std::bit_cast<decltype(header_.type_metadata)>(position);
  return position.offset;
}

}  // namespace vela::runtime
