#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

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

// 编辑器文档拥有 UTF-8 文本及行索引；UTF-16 只用于协议位置转换，不改变语言字符串语义。
class SourceText final {
 public:
  SourceText() = default;
  [[nodiscard]] static std::expected<SourceText, std::string> Create(std::string text);
  [[nodiscard]] std::string_view text() const noexcept { return text_; }
  [[nodiscard]] std::size_t line_count() const noexcept { return lines_.size(); }
  [[nodiscard]] std::expected<std::size_t, std::string> OffsetAt(TextPosition position,
                                                                 PositionEncoding encoding) const;
  [[nodiscard]] TextPosition PositionAt(std::size_t offset, PositionEncoding encoding) const;
  [[nodiscard]] std::expected<void, std::string> Replace(TextRange range,
                                                         std::string_view text,
                                                         PositionEncoding encoding);

 private:
  void IndexLines();
  [[nodiscard]] std::size_t LineEnd(std::size_t line) const;
  std::string text_;
  std::vector<std::size_t> lines_{0};
};

}  // namespace vela::tooling
