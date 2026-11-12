#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/byte_access.h"
#include "base/execution_status.h"
#include "base/regexp.h"
#include "runtime/isolate/isolate.h"

namespace vela {
namespace {

TEST(HostBuiltinTest, UnicodeAndBuilderResultsSurviveTheirSourceScope) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  std::optional<Local<Array<String>>> graphemes;
  std::optional<Local<String>> built;
  {
    HandleScope child(isolate);
    const auto text = CreateString(child, "é🌍");
    const auto builder = CreateMutableString(child);
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(builder.has_value());
    EXPECT_EQ(StringLength(*text), 3);
    EXPECT_EQ(StringByteLength(*text), 7);
    const auto count = StringGraphemeLength(child, *text);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(*count, 2);
    const auto points = StringCodePoints(child, *text);
    ASSERT_TRUE(points.has_value());
    EXPECT_EQ(points->Size(), 3);
    const auto earth = points->Get(child, 2);
    ASSERT_TRUE(earth.has_value());
    EXPECT_EQ(*earth, 0x1f30d);
    const auto groups = StringGraphemes(scope, *text);
    ASSERT_TRUE(groups.has_value());
    graphemes = *groups;
    ASSERT_TRUE(AppendString(child, *builder, *text));
    const auto output = BuildString(scope, *builder);
    ASSERT_TRUE(output.has_value());
    built = *output;
    const auto first = StringGraphemeSlice(child, *text, 0, 1);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->Utf8(), "é");
    const auto invalid = StringAt(child, *text, 3);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().status, ExecutionStatus::IndexOutOfBounds);
  }
  isolate.heap().Collect();
  EXPECT_EQ(CopyString(*built), "é🌍");
  const auto last = graphemes->Get(scope, 1);
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->Utf8(), "🌍");
  const auto index = StringIndexOf(scope, *built, *last);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(*index, 2);
}

TEST(HostBuiltinTest, StringOperationsUseCodePointRangesAndUnicodeCase) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  std::optional<Local<String>> slice;
  std::optional<Local<String>> upper;
  std::optional<Local<String>> lower;
  {
    HandleScope child(isolate);
    const auto text = CreateString(child, "Straße 🌍");
    const auto prefix = CreateString(child, "Stra");
    const auto suffix = CreateString(child, "🌍");
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(prefix.has_value());
    ASSERT_TRUE(suffix.has_value());
    const auto included = StringIncludes(child, *text, *suffix);
    const auto starts = StringStartsWith(child, *text, *prefix);
    const auto ends = StringEndsWith(child, *text, *suffix);
    const auto missing = StringIncludes(child, *prefix, *suffix);
    const auto wrong_start = StringStartsWith(child, *text, *suffix);
    const auto wrong_end = StringEndsWith(child, *text, *prefix);
    ASSERT_TRUE(included.has_value());
    ASSERT_TRUE(starts.has_value());
    ASSERT_TRUE(ends.has_value());
    ASSERT_TRUE(missing.has_value());
    ASSERT_TRUE(wrong_start.has_value());
    ASSERT_TRUE(wrong_end.has_value());
    EXPECT_TRUE(*included);
    EXPECT_TRUE(*starts);
    EXPECT_TRUE(*ends);
    EXPECT_FALSE(*missing);
    EXPECT_FALSE(*wrong_start);
    EXPECT_FALSE(*wrong_end);
    const auto part = StringSlice(scope, *text, 4, 8);
    const auto uppercase = StringToUpperCase(scope, *text);
    const auto lowercase = StringToLowerCase(scope, *text);
    ASSERT_TRUE(part.has_value());
    ASSERT_TRUE(uppercase.has_value());
    ASSERT_TRUE(lowercase.has_value());
    slice = *part;
    upper = *uppercase;
    lower = *lowercase;
  }
  isolate.heap().Collect();
  EXPECT_EQ(slice->Utf8(), "ße 🌍");
  EXPECT_EQ(upper->Utf8(), "STRASSE 🌍");
  EXPECT_EQ(lower->Utf8(), "straße 🌍");
}

TEST(HostBuiltinTest, ByteArrayCopiesValidateValuesAndSlicesHaveIndependentStorage) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  std::optional<Local<ByteArray>> slice;
  {
    HandleScope child(isolate);
    const auto values = CreateArray<std::int64_t>(child);
    ASSERT_TRUE(values.has_value());
    for (const auto value : {0, 127, 128, 256}) {
      ASSERT_TRUE(values->Push(child, value));
    }
    const auto invalid = ByteArrayFrom(child, *values);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().status, ExecutionStatus::InvalidNumericConversion);
    ASSERT_TRUE(values->Set(child, 3, 255));
    const auto bytes = ByteArrayFrom(child, *values);
    ASSERT_TRUE(bytes.has_value());
    ASSERT_TRUE(values->Set(child, 3, 0));
    EXPECT_EQ(CopyBytes(*bytes), (std::vector<std::uint8_t>{0, 127, 128, 255}));
    const auto copied = ByteArraySlice(scope, *bytes, -2, 4);
    ASSERT_TRUE(copied.has_value());
    slice = *copied;
    ASSERT_TRUE(ByteArraySet(child, *bytes, 2, 1));
    const auto invalid_value = ByteArraySet(child, *bytes, 2, -1);
    const auto invalid_index = ByteArraySet(child, *bytes, 4, 0);
    ASSERT_FALSE(invalid_value);
    ASSERT_FALSE(invalid_index);
    EXPECT_EQ(invalid_value.error().status, ExecutionStatus::InvalidNumericConversion);
    EXPECT_EQ(invalid_index.error().status, ExecutionStatus::IndexOutOfBounds);
    EXPECT_EQ(CopyBytes(*bytes), (std::vector<std::uint8_t>{0, 127, 1, 255}));
  }
  isolate.heap().Collect();
  EXPECT_EQ(CopyBytes(*slice), (std::vector<std::uint8_t>{128, 255}));
  const auto data = CreateByteData(scope, *slice);
  ASSERT_TRUE(data.has_value());
  const auto signed_value = ByteDataGetInt8(scope, *data, 0);
  const auto unsigned_value = ByteDataGetUint8(scope, *data, 0);
  ASSERT_TRUE(signed_value.has_value());
  ASSERT_TRUE(unsigned_value.has_value());
  EXPECT_EQ(*signed_value, -128);
  EXPECT_EQ(*unsigned_value, 128);
  ASSERT_TRUE(ByteDataSetInt8(scope, *data, 0, -1));
  const auto rejected = ByteDataSetInt8(scope, *data, 0, 128);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, ExecutionStatus::InvalidNumericConversion);
  EXPECT_EQ(CopyBytes(*slice), (std::vector<std::uint8_t>{255, 255}));
}

TEST(HostBuiltinTest, IntegerByteAccessPreservesSignednessWidthAndByteOrder) {
  struct Access {
    Result<std::int64_t> (*read)(HandleScope&, Local<ByteData>, std::int64_t, ByteOrder);
    Result<void> (*write)(HandleScope&, Local<ByteData>, std::int64_t, std::int64_t, ByteOrder);
    std::int64_t value;
    std::vector<std::uint8_t> bytes;
  };
  const std::array cases{
      Access{
          .read = ByteDataGetInt16,
          .write = ByteDataSetInt16,
          .value = -2,
          .bytes = {0xff, 0xfe},
      },
      Access{
          .read = ByteDataGetUint16,
          .write = ByteDataSetUint16,
          .value = 65534,
          .bytes = {0xff, 0xfe},
      },
      Access{
          .read = ByteDataGetInt32,
          .write = ByteDataSetInt32,
          .value = -2,
          .bytes = {0xff, 0xff, 0xff, 0xfe},
      },
      Access{
          .read = ByteDataGetUint32,
          .write = ByteDataSetUint32,
          .value = 0xf1020304,
          .bytes = {0xf1, 0x02, 0x03, 0x04},
      },
      Access{
          .read = ByteDataGetInt64,
          .write = ByteDataSetInt64,
          .value = std::numeric_limits<std::int64_t>::min() + 255,
          .bytes = {0x80, 0, 0, 0, 0, 0, 0, 0xff},
      },
  };
  runtime::Isolate isolate(1);
  for (const auto& access : cases) {
    for (const auto order : {ByteOrder::Big, ByteOrder::Little}) {
      HandleScope scope(isolate);
      auto expected = access.bytes;
      if (order == ByteOrder::Little) {
        std::ranges::reverse(expected);
      }
      const auto width = static_cast<std::int64_t>(expected.size());
      const auto bytes = CreateByteArray(scope, width + 2);
      ASSERT_TRUE(bytes.has_value());
      const auto view = CreateByteData(scope, *bytes, 1, width);
      ASSERT_TRUE(view.has_value());
      ASSERT_TRUE(access.write(scope, *view, 0, access.value, order));
      const auto value = access.read(scope, *view, 0, order);
      ASSERT_TRUE(value.has_value());
      EXPECT_EQ(*value, access.value);
      EXPECT_EQ(CopyBytes(*view), expected);
      const auto outside = access.read(scope, *view, 1, order);
      const auto negative = access.write(scope, *view, -1, access.value, order);
      ASSERT_FALSE(outside);
      ASSERT_FALSE(negative);
      EXPECT_EQ(outside.error().status, ExecutionStatus::IndexOutOfBounds);
      EXPECT_EQ(negative.error().status, ExecutionStatus::IndexOutOfBounds);
      EXPECT_EQ(CopyBytes(*view), expected);
      const auto storage = CopyBytes(*bytes);
      EXPECT_EQ(storage.front(), 0);
      EXPECT_EQ(storage.back(), 0);
    }
  }
}

TEST(HostBuiltinTest, FloatingByteAccessUsesIeeeEncodingAndByteOrder) {
  struct Access {
    Result<double> (*read)(HandleScope&, Local<ByteData>, std::int64_t, ByteOrder);
    Result<void> (*write)(HandleScope&, Local<ByteData>, std::int64_t, double, ByteOrder);
    double value;
    std::vector<std::uint8_t> bytes;
  };
  const std::array cases{
      Access{
          .read = ByteDataGetFloat32,
          .write = ByteDataSetFloat32,
          .value = 1.5,
          .bytes = {0x3f, 0xc0, 0, 0},
      },
      Access{
          .read = ByteDataGetFloat64,
          .write = ByteDataSetFloat64,
          .value = -1.5,
          .bytes = {0xbf, 0xf8, 0, 0, 0, 0, 0, 0},
      },
  };
  runtime::Isolate isolate(1);
  for (const auto& access : cases) {
    for (const auto order : {ByteOrder::Big, ByteOrder::Little}) {
      HandleScope scope(isolate);
      auto expected = access.bytes;
      if (order == ByteOrder::Little) {
        std::ranges::reverse(expected);
      }
      const auto bytes = CreateByteArray(scope, static_cast<std::int64_t>(expected.size()));
      ASSERT_TRUE(bytes.has_value());
      const auto view = CreateByteData(scope, *bytes);
      ASSERT_TRUE(view.has_value());
      ASSERT_TRUE(access.write(scope, *view, 0, access.value, order));
      const auto value = access.read(scope, *view, 0, order);
      ASSERT_TRUE(value.has_value());
      EXPECT_EQ(*value, access.value);
      EXPECT_EQ(CopyBytes(*bytes), expected);
    }
  }
}

TEST(HostBuiltinTest, ByteViewsKeepTheirOwnerAndUseLanguageRangeAndEndianRules) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  std::optional<Local<ByteData>> view;
  {
    HandleScope child(isolate);
    const auto bytes = CreateByteArray(child, 16);
    ASSERT_TRUE(bytes.has_value());
    const auto data = CreateByteData(scope, *bytes, 4, 8);
    ASSERT_TRUE(data.has_value());
    view = *data;
    ASSERT_TRUE(ByteDataSetInt32(child, *view, 0, 0x01020304));
    const auto first = ByteArrayGet(child, *bytes, 4);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 1);
    const auto swapped = ByteDataGetInt32(child, *view, 0, ByteOrder::Little);
    ASSERT_TRUE(swapped.has_value());
    EXPECT_EQ(*swapped, 0x04030201);
    const auto rejected = ByteDataSetUint8(child, *view, 0, 256);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().status, ExecutionStatus::InvalidNumericConversion);
    const auto unchanged = ByteDataGetUint8(child, *view, 0);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(*unchanged, 1);
    const auto outside = ByteDataGetFloat64(child, *view, 1);
    ASSERT_FALSE(outside);
    EXPECT_EQ(outside.error().status, ExecutionStatus::IndexOutOfBounds);
  }
  isolate.heap().Collect();
  EXPECT_EQ(ByteDataLength(*view), 8);
  ASSERT_TRUE(ByteDataSetFloat32(scope, *view, 4, 1.5, ByteOrder::Little));
  const auto number = ByteDataGetFloat32(scope, *view, 4, ByteOrder::Little);
  ASSERT_TRUE(number.has_value());
  EXPECT_EQ(*number, 1.5);
  const auto copied = CopyBytes(*view);
  EXPECT_EQ(copied.front(), 1);
  const auto bytes = CopyToByteArray(scope, copied);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(ByteArrayLength(*bytes), 8);
  EXPECT_EQ(CopyBytes(*bytes), copied);
}

TEST(HostBuiltinTest, RegExpCapturesRetainInputPatternAndNullableGroupTypes) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  std::optional<Local<RegExpMatch>> match;
  {
    HandleScope child(isolate);
    const auto source = CreateString(child, "(?<word>é)(x)?");
    const auto input = CreateString(child, "🌍é");
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(input.has_value());
    const auto pattern = CreateRegExp(child, *source);
    ASSERT_TRUE(pattern.has_value()) << pattern.error().message;
    const auto found = RegExpFirstMatch(scope, *pattern, *input);
    ASSERT_TRUE(found.has_value());
    if (!*found) {
      FAIL() << "expected a regular expression match";
    }
    match = **found;
    const auto all = RegExpAllMatches(child, *pattern, *input);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->Size(), 1);
    const auto missing = RegExpMatchGroup(child, *match, 2);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(*missing);
    const auto names = RegExpMatchGroupNames(child, *match);
    ASSERT_TRUE(names.has_value());
    const auto name = names->Get(child, 0);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name->Utf8(), "word");
  }
  isolate.heap().Collect();
  const auto input = RegExpMatchInput(scope, *match);
  const auto pattern = RegExpMatchPattern(scope, *match);
  const auto count = RegExpMatchGroupCount(scope, *match);
  ASSERT_TRUE(input.has_value());
  ASSERT_TRUE(pattern.has_value());
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(input->Utf8(), "🌍é");
  EXPECT_EQ(*count, 2);
  const auto source = RegExpPattern(scope, *pattern);
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(source->Utf8(), "(?<word>é)(x)?");
  const auto start = RegExpMatchStart(scope, *match);
  const auto end = RegExpMatchEnd(scope, *match);
  ASSERT_TRUE(start.has_value());
  ASSERT_TRUE(end.has_value());
  EXPECT_EQ(*start, 1);
  EXPECT_EQ(*end, 2);
  const auto name = CreateString(scope, "word");
  ASSERT_TRUE(name.has_value());
  const auto captured = RegExpMatchNamedGroup(scope, *match, *name);
  ASSERT_TRUE(captured.has_value());
  if (!*captured) {
    FAIL() << "expected the named capture word";
  }
  EXPECT_EQ((**captured).Utf8(), "é");
  const auto indices = CreateArray<std::int64_t>(scope);
  ASSERT_TRUE(indices.has_value());
  ASSERT_TRUE(indices->Push(scope, 1));
  ASSERT_TRUE(indices->Push(scope, 2));
  const auto groups = RegExpMatchGroups(scope, *match, *indices);
  ASSERT_TRUE(groups.has_value());
  const auto missing = groups->Get(scope, 1);
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(*missing);
}

TEST(HostBuiltinTest, RegExpOptionsControlSearchAndPrefixCodePointOffsets) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto source = CreateString(scope, "^(?<letter>é).");
  const auto input = CreateString(scope, "x\nÉ\n");
  ASSERT_TRUE(source.has_value());
  ASSERT_TRUE(input.has_value());
  const std::array cases{
      RegExpOptions{},
      RegExpOptions{.multi_line = true},
      RegExpOptions{.case_sensitive = false},
      RegExpOptions{.dot_all = true},
      RegExpOptions{.multi_line = true, .case_sensitive = false, .dot_all = true},
  };
  for (const auto options : cases) {
    const auto pattern = CreateRegExp(scope, *source, options);
    ASSERT_TRUE(pattern.has_value());
    const auto multi_line = RegExpIsMultiLine(scope, *pattern);
    const auto case_sensitive = RegExpIsCaseSensitive(scope, *pattern);
    const auto dot_all = RegExpIsDotAll(scope, *pattern);
    const auto unicode = RegExpIsUnicode(scope, *pattern);
    const auto found = RegExpHasMatch(scope, *pattern, *input);
    ASSERT_TRUE(multi_line.has_value());
    ASSERT_TRUE(case_sensitive.has_value());
    ASSERT_TRUE(dot_all.has_value());
    ASSERT_TRUE(unicode.has_value());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*multi_line, options.multi_line);
    EXPECT_EQ(*case_sensitive, options.case_sensitive);
    EXPECT_EQ(*dot_all, options.dot_all);
    EXPECT_TRUE(*unicode);
    const auto should_match = options.multi_line && !options.case_sensitive && options.dot_all;
    EXPECT_EQ(*found, should_match);
    const auto prefix = RegExpMatchAsPrefix(scope, *pattern, *input);
    const auto match = RegExpMatchAsPrefix(scope, *pattern, *input, 2);
    const auto text = RegExpStringMatch(scope, *pattern, *input);
    ASSERT_TRUE(prefix.has_value());
    ASSERT_TRUE(match.has_value());
    ASSERT_TRUE(text.has_value());
    EXPECT_FALSE(*prefix);
    ASSERT_EQ(match->has_value(), should_match);
    ASSERT_EQ(text->has_value(), should_match);
    if (*match) {
      const auto start = RegExpMatchStart(scope, **match);
      ASSERT_TRUE(start.has_value());
      EXPECT_EQ(*start, 2);
      if (!*text) {
        FAIL() << "missing matched text";
      }
      EXPECT_EQ((**text).Utf8(), "É\n");
    }
  }
}

TEST(HostBuiltinTest, EscapedRegExpMatchesLiteralMetacharacters) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto literal = CreateString(scope, "a+b[0].");
  const auto other = CreateString(scope, "ab0x");
  ASSERT_TRUE(literal.has_value());
  ASSERT_TRUE(other.has_value());
  const auto escaped = EscapeRegExp(scope, *literal);
  ASSERT_TRUE(escaped.has_value());
  const auto pattern = CreateRegExp(scope, *escaped);
  ASSERT_TRUE(pattern.has_value());
  const auto matches = RegExpHasMatch(scope, *pattern, *literal);
  const auto misses = RegExpHasMatch(scope, *pattern, *other);
  ASSERT_TRUE(matches.has_value());
  ASSERT_TRUE(misses.has_value());
  EXPECT_TRUE(*matches);
  EXPECT_FALSE(*misses);
}

TEST(HostBuiltinTest, BuiltinFailuresRejectForeignInputsAndPreserveAllocationStatus) {
  runtime::Isolate isolate(1);
  runtime::Isolate foreign(1);
  HandleScope scope(isolate);
  HandleScope other(foreign);
  const auto text = CreateString(other, "foreign");
  ASSERT_TRUE(text.has_value());
  const auto rejected = CreateRegExp(scope, *text);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, ExecutionStatus::InvalidIsolate);
  const auto source = CreateString(scope, "(");
  ASSERT_TRUE(source.has_value());
  const auto invalid = CreateRegExp(scope, *source);
  ASSERT_FALSE(invalid);
  EXPECT_FALSE(invalid.error().message.empty());
  auto& heap = isolate.heap();
  heap.SetAllocationLimit(heap.allocated_bytes());
  const auto failed = CreateByteArray(scope, 128);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().status, ExecutionStatus::OutOfMemory);
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
}

}  // namespace
}  // namespace vela
