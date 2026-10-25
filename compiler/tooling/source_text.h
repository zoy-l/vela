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
  auto operator==(const TextPosition&) const -> bool = default;
};

struct TextRange final {
  TextPosition start;
  TextPosition end;
};

// 编辑器文档拥有 UTF-8 文本及行索引；UTF-16 只用于协议位置转换，不改变语言字符串语义。
class SourceText final {
 public:
  SourceText() = default;
  [[nodiscard]] static auto Create(std::string text) -> std::expected<SourceText, std::string>;
  [[nodiscard]] auto text() const noexcept -> std::string_view { return text_; }
  [[nodiscard]] auto line_count() const noexcept -> std::size_t { return lines_.size(); }
  [[nodiscard]] auto OffsetAt(TextPosition position, PositionEncoding encoding) const
      -> std::expected<std::size_t, std::string>;
  [[nodiscard]] auto PositionAt(std::size_t offset, PositionEncoding encoding) const
      -> TextPosition;
  [[nodiscard]] auto Replace(TextRange range, std::string_view text, PositionEncoding encoding)
      -> std::expected<void, std::string>;

 private:
  void IndexLines();
  [[nodiscard]] auto LineEnd(std::size_t line) const -> std::size_t;
  std::string text_;
  std::vector<std::size_t> lines_{0};
};

}  // namespace vela::tooling
