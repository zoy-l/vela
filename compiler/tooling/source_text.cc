#include "compiler/tooling/source_text.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "base/source_lines.h"
#include "base/source_location.h"
#include "base/utf8.h"

namespace vela::tooling {
namespace {

std::size_t Utf8Width(std::string_view text, std::size_t offset) {
  const auto point = ReadUtf8Prefix(text.substr(offset));
  // 非法磁盘字节按一个替代字符计数，编辑器输入仍在 Create / Replace 处拒绝。
  return point ? point->width : 1;
}

std::size_t Utf8Boundary(std::string_view text, std::size_t byte_limit, std::size_t line_begin) {
  constexpr std::size_t kMaximumPrefix = 3;
  const auto prefix = std::min(kMaximumPrefix, byte_limit - line_begin);
  for (std::size_t distance = 1; distance <= prefix; ++distance) {
    const auto point = ReadUtf8Prefix(text.substr(byte_limit - distance));
    if (point && point->width > distance) {
      return byte_limit - distance;
    }
  }
  return byte_limit;
}

std::size_t Utf16Width(std::size_t bytes) {
  return bytes == 4 ? 2 : 1;
}

}  // namespace

std::expected<SourceText, std::string> SourceText::Create(std::string text) {
  if (const auto invalid = InvalidUtf8Offset(text)) {
    return std::unexpected("document contains invalid UTF-8 at byte " + std::to_string(*invalid));
  }
  SourceText result;
  result.text_ = std::move(text);
  result.index_.Reset(result.text_);
  return result;
}

void SourceLineIndex::Reset(std::string_view text) {
  lines_ = IndexSourceLines(text);
}

std::size_t SourceLineIndex::LineEnd(std::string_view text, std::size_t line) const {
  if (line + 1 == lines_->size()) {
    return text.size();
  }
  auto end = lines_->at(line + 1) - 1;
  if (end > lines_->at(line) && text.at(end - 1) == '\r') {
    --end;
  }
  return end;
}

std::expected<std::size_t, std::string> SourceLineIndex::OffsetAt(std::string_view text,
                                                                  TextPosition position,
                                                                  PositionEncoding encoding) const {
  if (position.line >= lines_->size()) {
    return std::unexpected("document line is out of range");
  }
  auto offset = lines_->at(position.line);
  const auto end = LineEnd(text, position.line);
  if (encoding == PositionEncoding::Utf8) {
    const auto target = offset + std::min(position.character, end - offset);
    if (Utf8Boundary(text, target, offset) != target) {
      return std::unexpected("position splits a Unicode character");
    }
    return target;
  }
  std::size_t character = 0;
  while (offset < end && character < position.character) {
    const auto bytes = Utf8Width(text, offset);
    const auto width = Utf16Width(bytes);
    if (position.character - character < width) {
      return std::unexpected("position splits a Unicode character");
    }
    character += width;
    offset += bytes;
  }
  // LSP 要求超出行尾的 character 截到行尾；行号越界仍是非法编辑。
  return offset;
}

TextPosition SourceLineIndex::PositionAt(std::string_view text,
                                         std::size_t offset,
                                         PositionEncoding encoding) const {
  offset = std::min(offset, text.size());
  const auto& lines = *lines_;
  const auto next = std::ranges::upper_bound(lines, offset);
  const auto line = static_cast<std::size_t>(std::distance(lines.begin(), next) - 1);
  const auto end = std::min(offset, LineEnd(text, line));
  const auto begin = lines[line];
  if (encoding == PositionEncoding::Utf8) {
    return {.line = line, .character = Utf8Boundary(text, end, begin) - begin};
  }
  std::size_t character = 0;
  for (auto cursor = begin; cursor < end;) {
    const auto bytes = Utf8Width(text, cursor);
    if (bytes > end - cursor) {
      break;
    }
    character += Utf16Width(bytes);
    cursor += bytes;
  }
  return {.line = line, .character = character};
}

TextPosition SourceLineIndex::PositionAt(std::string_view text,
                                         SourceLocation location,
                                         PositionEncoding encoding) const {
  const auto& lines = *lines_;
  const auto line = std::min(static_cast<std::size_t>(location.line - 1), lines.size() - 1);
  const auto begin = lines.at(line);
  const auto bytes =
      std::min(static_cast<std::size_t>(location.column - 1), LineEnd(text, line) - begin);
  return PositionAt(text, begin + bytes, encoding);
}

TextRange SourceLineIndex::RangeAt(std::string_view text,
                                   std::size_t offset,
                                   std::size_t length,
                                   PositionEncoding encoding) const {
  return {
      .start = PositionAt(text, offset, encoding),
      .end = PositionAt(text, offset + length, encoding),
  };
}

std::expected<std::size_t, std::string> SourceText::OffsetAt(TextPosition position,
                                                             PositionEncoding encoding) const {
  return index_.OffsetAt(text_, position, encoding);
}

TextPosition SourceText::PositionAt(std::size_t offset, PositionEncoding encoding) const {
  return index_.PositionAt(text_, offset, encoding);
}

TextPosition SourceText::PositionAt(SourceLocation location, PositionEncoding encoding) const {
  return index_.PositionAt(text_, location, encoding);
}

std::expected<void, std::string> SourceText::Replace(TextRange range,
                                                     std::string_view text,
                                                     PositionEncoding encoding) {
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
  index_.Reset(text_);
  return {};
}

}  // namespace vela::tooling
