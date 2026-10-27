#include <array>
#include <cstdint>
#include <string>
#include <tuple>

#include "gtest/gtest.h"

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/unicode_operations.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

class UnicodeDestinationTest : public testing::TestWithParam<std::tuple<Builtin, std::uint32_t>> {};

TEST_P(UnicodeDestinationTest, RejectsWrongLengthBeforeChangingAnyElement) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  std::array<Value, 2> arguments;
  RootRange roots(heap, arguments.data(), arguments.size());
  arguments.front() = Value::String(heap.AllocateString("aé"));
  const auto [builtin, length] = GetParam();
  const auto kind = builtin == Builtin::StringCodePoints ? ValueKind::Integer : ValueKind::String;
  const auto* layout = isolate.CreateArrayLayout({.element = {.kind = kind}});
  auto* array = heap.AllocateArray(*layout, length);
  ASSERT_NE(array, nullptr);
  arguments.back() = Value::Object(array);
  EXPECT_EQ(ExecuteUnicodeBuiltin(builtin, arguments, heap).error(), ExecutionStatus::TypeError);
  const auto first = Heap::GetElement(*array, 0).value();
  if (builtin == Builtin::StringCodePoints) {
    EXPECT_EQ(first.AsInteger(), 0);
  } else {
    EXPECT_TRUE(first.IsNull());
  }
}

INSTANTIATE_TEST_SUITE_P(ShortAndLong,
                         UnicodeDestinationTest,
                         testing::Combine(testing::Values(Builtin::StringCodePoints,
                                                          Builtin::StringGraphemes),
                                          testing::Values(std::uint32_t{1}, std::uint32_t{3})));

TEST(UnicodeOperationsTest, GraphemeSliceUsesClusterBoundariesAndValidatesEnd) {
  Heap heap;
  std::array<Value, 3> arguments = {
      Value::String(heap.AllocateString("á👩‍💻z")),
      Value::Integer(1),
      Value::Integer(2),
  };
  RootRange roots(heap, arguments.data(), arguments.size());
  const auto slice = ExecuteUnicodeBuiltin(Builtin::StringGraphemeSlice, arguments, heap);
  ASSERT_TRUE(slice);
  EXPECT_EQ(slice->AsString()->value(), "👩‍💻");
  arguments.back() = Value::Integer(4);
  EXPECT_EQ(ExecuteUnicodeBuiltin(Builtin::StringGraphemeSlice, arguments, heap).error(),
            ExecutionStatus::IndexOutOfBounds);
  arguments.at(1) = Value::Integer(0);
  arguments.back() = Value::Integer(0);
  const auto empty = ExecuteUnicodeBuiltin(Builtin::StringGraphemeSlice, arguments, heap);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->AsString()->value().empty());
}

TEST(UnicodeOperationsTest, LongGraphemeFillKeepsEverySegmentAliveAcrossCollections) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  const std::string source(80, 'x');
  std::array<Value, 2> arguments;
  RootRange roots(heap, arguments.data(), arguments.size());
  arguments.front() = Value::String(heap.AllocateString(source));
  const auto* layout = isolate.CreateArrayLayout({.element = {.kind = ValueKind::String}});
  auto* array = heap.AllocateArray(*layout, static_cast<std::uint32_t>(source.size()));
  ASSERT_NE(array, nullptr);
  arguments.back() = Value::Object(array);
  ASSERT_TRUE(ExecuteUnicodeBuiltin(Builtin::StringGraphemes, arguments, heap));
  heap.Collect();
  const auto joined = heap.JoinArray(*array, "");
  ASSERT_TRUE(joined);
  EXPECT_EQ((*joined)->value(), source);
}

}  // namespace
}  // namespace vela::runtime
