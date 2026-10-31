#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

TEST(UnicodeTest, SegmentsCombiningMarksEmojiFlagsHangulIndicAndLineBreaks) {
  runtime::Isolate isolate(1);
  const auto entry = test::CompileCleanSource(R"(
func main(): bool {
  const text = "Ae\u0301\u{1F1E8}\u{1F1F3}\u{1F469}\u200D\u{1F4BB}\r\n\u1100\u1161\u11A8\u0915\u094D\u0937";
  const parts: string[] = text.graphemes();
  const points: int[] = "\u{1F600}".codePoints();
  const nested: string[][] = [parts];
  const numbers: int[][] = [points];
  return text.graphemeLength == 7 && parts.length == 7 &&
         parts[0] == "A" && parts[1] == "e\u0301" &&
         parts[2] == "\u{1F1E8}\u{1F1F3}" &&
         parts[3] == "\u{1F469}\u200D\u{1F4BB}" &&
         parts[4] == "\r\n" && parts[5] == "\u1100\u1161\u11A8" &&
         parts[6] == "\u0915\u094D\u0937" &&
         text.graphemeSlice(1, 3) == parts[1] + parts[2] &&
         text.graphemeSlice(7, 7) == "" &&
         nested[0].length == 7 && numbers[0][0].toString() == "128512" &&
         "".codePoints().length == 0 && "".graphemes().length == 0 &&
         "".graphemeSlice(0, 0) == "";
}
)",
                                        isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto retained = isolate.heap().object_count();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_TRUE(result.value.AsBoolean());
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), retained);
}

TEST(UnicodeTest, AllocatesExactDynamicArraysBeyondLiteralLengthEncoding) {
  runtime::Isolate isolate(1);
  const std::string source =
      "func main(): int { const text = \"" + std::string(1000, 'a') +
      "\"; const points = text.codePoints(); const graphemes = text.graphemes(); "
      "return points.length + graphemes.length + points[999]; }";
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 2097);
}

class GraphemeBoundsTest : public testing::TestWithParam<std::string_view> {};

TEST_P(GraphemeBoundsTest, RejectsInvalidSliceBounds) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(GetParam(), isolate);
  ASSERT_TRUE(compiled.Succeeded());
  EXPECT_EQ(test::Execute(*compiled.instance, "main").status,
            ExecutionStatus::IndexOutOfBounds);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries,
    GraphemeBoundsTest,
    testing::Values("func main(): string { return \"a\".graphemeSlice(-1, 0); }",
                    "func main(): string { return \"a\".graphemeSlice(1, 0); }",
                    "func main(): string { return \"a\".graphemeSlice(0, 2); }"));

}  // namespace
}  // namespace vela::compiler
