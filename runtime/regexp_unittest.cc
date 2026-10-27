#include <array>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#include <pcre2.h>
#include <cstdint>
#include "gtest/gtest.h"

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr auto kRepeatedSearches = 100;
constexpr auto kAmbiguousInputLength = 200;
constexpr auto kNativeTestBytes = 123;

class RegExpTest : public testing::Test {
 protected:
  std::expected<Value, RegExpFailure> Call(Builtin builtin,
                                           std::initializer_list<Value> arguments,
                                           const ArrayLayout* layout = nullptr) {
    return ExecuteRegExpBuiltin(builtin, arguments, heap(), layout);
  }

  void Compile(std::string_view source) {
    roots().at(0) = Value::String(heap().AllocateString(source));
    const auto compiled = Call(Builtin::NewRegExp, {
                                                       roots().at(0),
                                                       Value::Boolean(false),
                                                       Value::Boolean(true),
                                                       Value::Boolean(false),
                                                   });
    ASSERT_TRUE(compiled) << compiled.error().message;
    roots().at(0) = *compiled;
  }

  void Input(std::string_view text) {
    roots().at(1) = Value::String(heap().AllocateString(text));
    ASSERT_NE(roots().at(1).AsString(), nullptr);
  }

  Heap& heap() { return heap_; }
  std::array<Value, 4>& roots() { return roots_; }

 private:
  Heap heap_{1};
  std::array<Value, 4> roots_{};
  RootRange root_{heap_, roots_.data(), roots_.size()};
};

TEST_F(RegExpTest, HasMatchDoesNotAllocateGCObjectsOrRetainWorkBuffers) {
  Compile("(?<word>\\w+)");
  Input("😀中文42");
  heap().Collect();
  const auto objects = heap().object_count();
  const auto bytes = heap().allocated_bytes();
  const auto committed = heap().committed_bytes();
  for (int iteration = 0; iteration < kRepeatedSearches; ++iteration) {
    const auto result = Call(Builtin::RegExpHasMatch, {roots().at(0), roots().at(1)});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->AsBoolean());
  }
  EXPECT_EQ(heap().object_count(), objects);
  EXPECT_EQ(heap().allocated_bytes(), bytes);
  EXPECT_EQ(heap().committed_bytes(), committed);
}

TEST_F(RegExpTest, MatchAloneKeepsInputPatternAndCapturesAlive) {
  Compile("(?<letter>中)(x)?()");
  Input("😀中");
  const auto matched = Call(Builtin::RegExpFirstMatch, {roots().at(0), roots().at(1)});
  ASSERT_TRUE(matched);
  roots().at(2) = *matched;
  roots().at(0) = Value::Null();
  roots().at(1) = Value::Null();
  heap().Collect();
  const auto text = Call(Builtin::RegExpMatchGroup, {roots().at(2), Value::Integer(1)});
  ASSERT_TRUE(text);
  EXPECT_EQ(text->AsString()->value(), "中");
  EXPECT_TRUE(Call(Builtin::RegExpMatchGroup, {roots().at(2), Value::Integer(2)})->IsNull());
  const auto empty = Call(Builtin::RegExpMatchGroup, {roots().at(2), Value::Integer(3)});
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty->AsString()->value(), "");
  roots().at(2) = Value::Null();
  heap().Collect();
  heap().Collect();
  EXPECT_EQ(heap().allocated_bytes(), 0);
  EXPECT_EQ(heap().object_count(), 0);
}

TEST_F(RegExpTest, EmptyMatchesAdvanceOneUnicodeScalarAndIncludeTheEndBoundary) {
  Compile("(?=.)|$");
  Input("😀中");
  const ArrayLayout layout{
      .element = {.kind = ValueKind::Object, .object_kind = ObjectKind::RegExpMatch},
  };
  const auto matched =
      Call(Builtin::RegExpAllMatches,
           {roots().at(0), roots().at(1), Value::Integer(0), Value::Integer(0)}, &layout);
  ASSERT_TRUE(matched);
  roots().at(2) = *matched;
  auto& array = *matched->AsObject();
  ASSERT_EQ(Heap::ArrayLength(array), 3U);
  const auto middle = Heap::GetElement(array, 1).value();
  EXPECT_EQ(Call(Builtin::RegExpMatchStart, {middle})->AsInteger(), 1);
  const auto end = Heap::GetElement(array, 2).value();
  EXPECT_EQ(Call(Builtin::RegExpMatchStart, {end})->AsInteger(), 2);
}

TEST_F(RegExpTest, DebugDescriptionsExposeSourceAndCodePointRange) {
  Compile("(中)");
  Input("😀中");
  std::ostringstream pattern;
  WriteRegExp(pattern, *roots().at(0).AsObject());
  EXPECT_EQ(pattern.str(), "RegExp(pattern=(中))");
  const auto found = Call(Builtin::RegExpFirstMatch, {roots().at(0), roots().at(1)});
  ASSERT_TRUE(found);
  std::ostringstream match;
  WriteRegExp(match, *found->AsObject());
  EXPECT_EQ(match.str(), "RegExpMatch(start=1, end=2, groupCount=1)");
}

struct LimitCase final {
  std::string_view source;
  std::string_view message;
};

class RegExpLimitTest : public RegExpTest, public testing::WithParamInterface<LimitCase> {};

void ExpectLimit(const std::expected<Value, RegExpFailure>& result, std::string_view message) {
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().status, ExecutionStatus::RegExpLimitError);
  EXPECT_NE(result.error().message.find(message), std::string::npos);
}

TEST_P(RegExpLimitTest, LimitsAreRecoverableAndReleaseOperationMemory) {
  Compile(GetParam().source);
  Input(std::string(kAmbiguousInputLength, 'a') + '!');
  heap().Collect();
  const auto bytes = heap().allocated_bytes();
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto result = Call(Builtin::RegExpHasMatch, {roots().at(0), roots().at(1)});
    ExpectLimit(result, GetParam().message);
    EXPECT_EQ(heap().allocated_bytes(), bytes);
  }
  Compile("a");
  const auto recovered = Call(Builtin::RegExpHasMatch, {roots().at(0), roots().at(1)});
  ASSERT_TRUE(recovered);
  EXPECT_TRUE(recovered->AsBoolean());
}

INSTANTIATE_TEST_SUITE_P(
    InterpreterBudgets,
    RegExpLimitTest,
    testing::Values(
        LimitCase{.source = "(*NO_START_OPT)(*LIMIT_MATCH=1)(a+)+$", .message = "match limit"},
        LimitCase{.source = "(*NO_START_OPT)(*LIMIT_DEPTH=1)(a+)+$", .message = "depth limit"},
        LimitCase{.source = "(*NO_START_OPT)(*LIMIT_HEAP=0)(a+)+$", .message = "heap limit"}));

TEST_F(RegExpTest, ExecutionErrorPreservesPatternReusabilityAndReleasesBuffers) {
  Compile("(?R)");
  Input("");
  heap().Collect();
  const auto bytes = heap().allocated_bytes();
  const auto result = Call(Builtin::RegExpFirstMatch, {roots().at(0), roots().at(1)});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().status, ExecutionStatus::RegExpExecutionError);
  EXPECT_EQ(heap().allocated_bytes(), bytes);
  const auto repeated = Call(Builtin::RegExpHasMatch, {roots().at(0), roots().at(1)});
  ASSERT_FALSE(repeated);
  EXPECT_EQ(repeated.error().status, ExecutionStatus::RegExpExecutionError);
}

TEST_F(RegExpTest, AllMatchesFailureDoesNotExposePartialArray) {
  Compile("(*NO_START_OPT)(*LIMIT_MATCH=10)a|(?R)");
  Input("a!");
  const ArrayLayout layout{
      .element = {.kind = ValueKind::Object, .object_kind = ObjectKind::RegExpMatch},
  };
  const auto result =
      Call(Builtin::RegExpAllMatches,
           {roots().at(0), roots().at(1), Value::Integer(0), Value::Integer(0)}, &layout);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().status, ExecutionStatus::RegExpExecutionError);
  roots().at(0) = Value::Null();
  roots().at(1) = Value::Null();
  heap().Collect();
  heap().Collect();
  EXPECT_EQ(heap().allocated_bytes(), 0);
}

TEST(RegExpNativeStorageTest, ExactAccountingAndRepresentability) {
  Heap heap;
  const auto committed = heap.committed_bytes();
  auto* storage = heap.AllocateNative(kNativeTestBytes);
  ASSERT_NE(storage, nullptr);
  EXPECT_GE(heap.allocated_bytes(), kNativeTestBytes);
  EXPECT_EQ(heap.allocated_bytes(), heap.committed_bytes() - committed);
  heap.FreeNative(storage);
  EXPECT_EQ(heap.allocated_bytes(), 0);
  EXPECT_EQ(heap.committed_bytes(), committed);
  EXPECT_EQ(heap.AllocateNative(static_cast<std::size_t>(-1)), nullptr);
}

TEST(RegExpEngineTest, PinnedUnicodeInterpreterConfiguration) {
  constexpr auto kVersionCapacity = 32;
  std::array<PCRE2_UCHAR, kVersionCapacity> version{};
  EXPECT_GT(pcre2_config(PCRE2_CONFIG_UNICODE_VERSION, version.data()), 0);
  const void* bytes = version.data();
  EXPECT_EQ(std::string_view(static_cast<const char*>(bytes)), "17.0.0");
  std::uint32_t value = 0;
  EXPECT_EQ(pcre2_config(PCRE2_CONFIG_JIT, &value), 0);
  EXPECT_EQ(value, 0);
  constexpr auto kMatchLimit = 1000000;
  constexpr auto kDepthLimit = 1000;
  constexpr auto kHeapLimitKiB = 8192;
  pcre2_config(PCRE2_CONFIG_MATCHLIMIT, &value);
  EXPECT_EQ(value, kMatchLimit);
  pcre2_config(PCRE2_CONFIG_DEPTHLIMIT, &value);
  EXPECT_EQ(value, kDepthLimit);
  pcre2_config(PCRE2_CONFIG_HEAPLIMIT, &value);
  EXPECT_EQ(value, kHeapLimitKiB);
}

}  // namespace
}  // namespace vela::runtime
