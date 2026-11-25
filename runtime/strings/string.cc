#include "runtime/strings/string.h"

#include <bit>
#include <cstddef>
#include <cstdint>

#include "base/utf8.h"
#include "runtime/value/object.h"

namespace vela::runtime {
namespace {
constexpr std::uint8_t kTwoByteLead = 0xc0;
constexpr std::uint8_t kThreeByteLead = 0xe0;
constexpr std::uint8_t kFourByteLead = 0xf0;
}  // namespace

std::size_t StringObject::ByteOffset(std::uint32_t index) const noexcept {
  const auto bytes = value();
  const auto count = length();
  if (count == bytes.size()) {
    return index;
  }
  auto position = std::bit_cast<HeapObject::StringPosition>(header_.type_metadata);
  const auto width_at = [&bytes](std::uint32_t offset) {
    const auto first = static_cast<std::uint8_t>(bytes[offset]);
    return 1U + static_cast<std::uint32_t>(first >= kTwoByteLead) +
           static_cast<std::uint32_t>(first >= kThreeByteLead) +
           static_cast<std::uint32_t>(first >= kFourByteLead);
  };
  if (index == position.index + 1) {
    // 顺序遍历只前进一步，不必重复比较缓存与两端距离；末尾边界也由同一路径得到。
    position.offset += width_at(position.offset);
    position.index = index;
    header_.type_metadata = std::bit_cast<decltype(header_.type_metadata)>(position);
    return position.offset;
  }
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
    // 字节已验证为 UTF-8；这里只移动位置，不需要解码码点或读取其余载荷字节。
    position.offset += width_at(position.offset);
    ++position.index;
  }
  while (position.index > index) {
    --position.offset;
    while (IsContinuationByte(bytes[position.offset])) {
      --position.offset;
    }
    --position.index;
  }
  header_.type_metadata = std::bit_cast<decltype(header_.type_metadata)>(position);
  return position.offset;
}

}  // namespace vela::runtime
