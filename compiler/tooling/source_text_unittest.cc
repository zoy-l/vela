#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/tooling/source_text.h"

namespace vela::tooling {
namespace {

TEST(SourceTextTest, ConvertsUtf8AndUtf16PositionsWithCrLfAndSupplementaryCharacters) {
  const auto source = SourceText::Create("a中😀z\r\nnext\n");
  ASSERT_TRUE(source);
  EXPECT_EQ(source->line_count(), 3U);
  EXPECT_EQ(source->OffsetAt({.line = 0, .character = 4}, PositionEncoding::Utf16), 8U);
  EXPECT_EQ(source->OffsetAt({.line = 0, .character = 8}, PositionEncoding::Utf8), 8U);
  EXPECT_EQ(source->PositionAt(8, PositionEncoding::Utf16),
            (TextPosition{.line = 0, .character = 4}));
  EXPECT_EQ(source->PositionAt(16, PositionEncoding::Utf16),
            (TextPosition{.line = 2, .character = 0}));
  EXPECT_EQ(source->OffsetAt({.line = 0, .character = 1000}, PositionEncoding::Utf16), 9U);
}

TEST(SourceTextTest, RejectsPositionsSplittingUnicodeAndOutOfRangeLines) {
  const auto source = SourceText::Create("a中😀z");
  ASSERT_TRUE(source);
  EXPECT_FALSE(source->OffsetAt({.line = 0, .character = 3}, PositionEncoding::Utf16));
  EXPECT_FALSE(source->OffsetAt({.line = 0, .character = 2}, PositionEncoding::Utf8));
  EXPECT_FALSE(source->OffsetAt({.line = 1, .character = 0}, PositionEncoding::Utf16));
  EXPECT_EQ(source->PositionAt(2, PositionEncoding::Utf16),
            (TextPosition{.line = 0, .character = 1}));
}

TEST(SourceTextTest, AppliesSequentialEditsAndRebuildsTheLineIndex) {
  auto source = SourceText::Create("a中😀z\r\nnext\n");
  ASSERT_TRUE(source);
  ASSERT_TRUE(
      source->Replace({.start = {.line = 0, .character = 1}, .end = {.line = 0, .character = 4}},
                      "X", PositionEncoding::Utf16));
  EXPECT_EQ(source->text(), "aXz\r\nnext\n");
  ASSERT_TRUE(source->Replace({.start = {.line = 0}, .end = {.line = 1, .character = 4}}, "changed",
                              PositionEncoding::Utf16));
  EXPECT_EQ(source->text(), "changed\n");
  EXPECT_EQ(source->line_count(), 2U);
}

TEST(SourceTextTest, InvalidReplacementsPreserveTheDocument) {
  auto source = SourceText::Create("hello");
  ASSERT_TRUE(source);
  const std::string invalid(1, static_cast<char>(0xff));
  EXPECT_FALSE(SourceText::Create(invalid));
  EXPECT_FALSE(source->Replace({}, invalid, PositionEncoding::Utf16));
  EXPECT_FALSE(source->Replace({.start = {.character = 4}, .end = {.character = 1}}, "x",
                               PositionEncoding::Utf16));
  EXPECT_EQ(source->text(), "hello");
}

TEST(SourceTextTest, EmptyDocumentHasOneLineAndClampsEndPositions) {
  const SourceText source;
  EXPECT_EQ(source.line_count(), 1U);
  EXPECT_EQ(source.OffsetAt({}, PositionEncoding::Utf8), 0U);
  EXPECT_EQ(source.PositionAt(1000, PositionEncoding::Utf16), TextPosition());
}

}  // namespace
}  // namespace vela::tooling
