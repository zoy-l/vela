#include <array>
#include <string_view>

#include <utf8proc.h>
#include "gtest/gtest.h"

#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

struct CaseSample final {
  std::string_view source;
  std::string_view lower;
  std::string_view upper;
};

TEST(StringCaseTest, BackendUsesThePinnedUnicodeVersion) {
  EXPECT_EQ(std::string_view(utf8proc_unicode_version()), "18.0.0");
}

class StringCaseMappingTest : public testing::TestWithParam<CaseSample> {
 protected:
  static void Check(StringCase conversion, std::string_view expected) {
    const auto& sample = GetParam();
    Heap heap(1);
    const auto input = Value::String(heap.AllocateString(sample.source));
    RootRange input_root(heap, &input, 1);
    const auto output = Value::String(heap.ConvertStringCase(*input.AsString(), conversion));
    ASSERT_NE(output.AsString(), nullptr);
    RootRange output_root(heap, &output, 1);
    EXPECT_EQ(output.AsString()->value(), expected);
    auto* repeated = heap.ConvertStringCase(*output.AsString(), conversion);
    ASSERT_NE(repeated, nullptr);
    EXPECT_EQ(repeated->value(), expected);
    EXPECT_EQ(input.AsString()->value(), sample.source);
    heap.Collect();
    EXPECT_EQ(output.AsString()->value(), expected);
  }
};

TEST_P(StringCaseMappingTest, LowercaseIsCompleteAndSurvivesCollection) {
  Check(StringCase::Lower, GetParam().lower);
}

TEST_P(StringCaseMappingTest, UppercaseIsCompleteAndSurvivesCollection) {
  Check(StringCase::Upper, GetParam().upper);
}

INSTANTIATE_TEST_SUITE_P(UnicodeDefaults,
                         StringCaseMappingTest,
                         testing::ValuesIn(std::to_array<CaseSample>({
                             {"", "", ""},
                             {"Vela 42!", "vela 42!", "VELA 42!"},
                             {"straße", "straße", "STRASSE"},
                             {"İ", "i\xcc\x87", "İ"},
                             {"ΟΣ", "ος", "ΟΣ"},
                             {"ΟΣΑ", "οσα", "ΟΣΑ"},
                             {"Σ", "σ", "Σ"},
                             {"AΣ'", "aς'", "AΣ'"},
                             {"AΣ'A", "aσ'a", "AΣ'A"},
                             {"A\xcc\x81Σ\xcc\x81", "a\xcc\x81ς\xcc\x81", "A\xcc\x81Σ\xcc\x81"},
                             {"\xcd\x85Σ", "\xcd\x85σ", "ΙΣ"},
                             {"A\xcd\x85Σ", "a\xcd\x85ς", "AΙΣ"},
                             {"AΣ\xcd\x85", "aς\xcd\x85", "AΣΙ"},
                             {"ﬃ", "ﬃ", "FFI"},
                             {"𐐀𐐨", "𐐨𐐨", "𐐀𐐀"},
                             {"中文🙂", "中文🙂", "中文🙂"},
                             {"e\xcc\x81", "e\xcc\x81", "E\xcc\x81"},
                             {std::string_view("A\0Σ", 4), std::string_view("a\0σ", 4),
                              std::string_view("A\0Σ", 4)},
                         })));

TEST(StringCaseTest, AllocatesOnlyTheFinalStringAndReusesUnchangedText) {
  Heap heap;
  const auto input = Value::String(heap.AllocateString("straße"));
  RootRange root(heap, &input, 1);
  const auto before = heap.object_count();
  EXPECT_EQ(heap.ConvertStringCase(*input.AsString(), StringCase::Lower), input.AsString());
  EXPECT_EQ(heap.object_count(), before);
  auto* output = heap.ConvertStringCase(*input.AsString(), StringCase::Upper);
  ASSERT_NE(output, nullptr);
  EXPECT_EQ(output->value(), "STRASSE");
  EXPECT_EQ(heap.object_count(), before + 1);
}

}  // namespace
}  // namespace vela::runtime
