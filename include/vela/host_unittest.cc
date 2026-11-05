#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/byte_access.h"
#include "base/execution_status.h"
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
