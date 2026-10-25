#include "compiler/tooling/source_text.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "base/utf8.h"

namespace vela::tooling {
namespace {

constexpr unsigned char kAsciiLimit = 0x80;
constexpr unsigned char kThreeByteFirst = 0xe0;
constexpr unsigned char kFourByteFirst = 0xf0;

auto Utf8Width(char character) -> std::size_t {
  const auto byte = static_cast<unsigned char>(character);
  if (byte < kAsciiLimit) {
    return 1;
  }
  if (byte < kThreeByteFirst) {
    return 2;
  }
  if (byte < kFourByteFirst) {
    return 3;
  }
  return 4;
}

auto PositionWidth(std::size_t bytes, PositionEncoding encoding) -> std::size_t {
  if (encoding == PositionEncoding::Utf8) {
    return bytes;
  }
  return bytes == 4 ? 2 : 1;
}

}  // namespace

auto SourceText::Create(std::string text) -> std::expected<SourceText, std::string> {
  if (const auto invalid = InvalidUtf8Offset(text)) {
    return std::unexpected("document contains invalid UTF-8 at byte " + std::to_string(*invalid));
  }
  SourceText result;
  result.text_ = std::move(text);
  result.IndexLines();
  return result;
}

void SourceText::IndexLines() {
  lines_.assign(1, 0);
  for (std::size_t offset = 0; offset < text_.size(); ++offset) {
    if (text_.at(offset) == '\n') {
      lines_.push_back(offset + 1);
    }
  }
}

auto SourceText::LineEnd(std::size_t line) const -> std::size_t {
  if (line + 1 == lines_.size()) {
    return text_.size();
  }
  auto end = lines_.at(line + 1) - 1;
  if (end > lines_.at(line) && text_.at(end - 1) == '\r') {
    --end;
  }
  return end;
}

auto SourceText::OffsetAt(TextPosition position, PositionEncoding encoding) const
    -> std::expected<std::size_t, std::string> {
  if (position.line >= lines_.size()) {
    return std::unexpected("document line is out of range");
  }
  auto offset = lines_.at(position.line);
  const auto end = LineEnd(position.line);
  std::size_t character = 0;
  while (offset < end && character < position.character) {
    const auto bytes = Utf8Width(text_.at(offset));
    const auto width = PositionWidth(bytes, encoding);
    if (position.character - character < width) {
      return std::unexpected("position splits a Unicode character");
    }
    character += width;
    offset += bytes;
  }
  // LSP 要求超出行尾的 character 截到行尾；行号越界仍是非法编辑。
  return offset;
}

auto SourceText::PositionAt(std::size_t offset, PositionEncoding encoding) const -> TextPosition {
  offset = std::min(offset, text_.size());
  const auto next = std::ranges::upper_bound(lines_, offset);
  const auto line = static_cast<std::size_t>(std::distance(lines_.begin(), next) - 1);
  const auto end = std::min(offset, LineEnd(line));
  std::size_t character = 0;
  for (auto cursor = lines_.at(line); cursor < end;) {
    const auto bytes = Utf8Width(text_.at(cursor));
    if (bytes > end - cursor) {
      break;
    }
    character += PositionWidth(bytes, encoding);
    cursor += bytes;
  }
  return {.line = line, .character = character};
}

auto SourceText::Replace(TextRange range, std::string_view text, PositionEncoding encoding)
    -> std::expected<void, std::string> {
  const auto start = OffsetAt(range.start, encoding);
  const auto end = OffsetAt(range.end, encoding);
  if (!start || !end) {
    return std::unexpected(!start ? start.error() : end.error());
  }
  if (*end < *start) {
    return std::unexpected("document edit has a reversed range");
  }
  if (InvalidUtf8Offset(text)) {
    return std::unexpected("replacement text is not valid UTF-8");
  }
  text_.replace(*start, *end - *start, text);
  IndexLines();
  return {};
}

}  // namespace vela::tooling
