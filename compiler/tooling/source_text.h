#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "base/source_location.h"

namespace vela::tooling {

enum class PositionEncoding : std::uint8_t { Utf8, Utf16 };

struct TextPosition final {
  std::size_t line = 0;
  std::size_t character = 0;
  bool operator==(const TextPosition&) const = default;
};

struct TextRange final {
  TextPosition start;
  TextPosition end;
};

// 索引与文本所有权分离；调用方始终提供建立索引时的文本，磁盘错误字节也可定位。
class SourceLineIndex final {
 public:
  SourceLineIndex() = default;
  explicit SourceLineIndex(std::string_view text);
  void Reset(std::string_view text);
  [[nodiscard]] std::size_t line_count() const noexcept { return lines_.size(); }
  [[nodiscard]] std::expected<std::size_t, std::string> OffsetAt(std::string_view text,
                                                                 TextPosition position,
                                                                 PositionEncoding encoding) const;
  [[nodiscard]] TextPosition PositionAt(std::string_view text,
                                        std::size_t offset,
                                        PositionEncoding encoding) const;
  [[nodiscard]] TextPosition PositionAt(std::string_view text,
                                        SourceLocation location,
                                        PositionEncoding encoding) const;

 private:
  [[nodiscard]] std::size_t LineEnd(std::string_view text, std::size_t line) const;
  std::vector<std::size_t> lines_{0};
};

// 编辑器文档拥有 UTF-8 文本及行索引；UTF-16 只用于协议位置转换，不改变语言字符串语义。
class SourceText final {
 public:
  SourceText() = default;
  [[nodiscard]] static std::expected<SourceText, std::string> Create(std::string text);
  [[nodiscard]] std::string_view text() const noexcept { return text_; }
  [[nodiscard]] std::size_t line_count() const noexcept { return index_.line_count(); }
  [[nodiscard]] std::expected<std::size_t, std::string> OffsetAt(TextPosition position,
                                                                 PositionEncoding encoding) const;
  [[nodiscard]] TextPosition PositionAt(std::size_t offset, PositionEncoding encoding) const;
  [[nodiscard]] TextPosition PositionAt(SourceLocation location, PositionEncoding encoding) const;
  [[nodiscard]] std::expected<void, std::string> Replace(TextRange range,
                                                         std::string_view text,
                                                         PositionEncoding encoding);

 private:
  std::string text_;
  SourceLineIndex index_;
};

}  // namespace vela::tooling
