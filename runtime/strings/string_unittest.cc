#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/library/builtins.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"

namespace vela::runtime {
namespace {

constexpr double kRoundTripValue = 1.2345678901234567;

TEST(StringGcTest, StoresEmptyAndEmbeddedNullBytes) {
  Heap heap;
  auto* empty = heap.AllocateString("");
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->value().empty());
  EXPECT_EQ(empty->length(), 0U);
  EXPECT_EQ(empty->ByteOffset(0), 0U);
  constexpr std::string_view kBytes("a\0b", 3);
  auto* joined = heap.AllocateString(kBytes, kBytes);
  ASSERT_NE(joined, nullptr);
  EXPECT_EQ(joined->value(), std::string_view("a\0ba\0b", 6));
  EXPECT_EQ(joined->length(), 6U);
  EXPECT_EQ(joined->ByteOffset(5), 5U);
  EXPECT_EQ(heap.object_count(), 2U);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

TEST(StringGcTest, ByteReplacementAllocatesOnlyOneResultWithAliasedRootedInputs) {
  Heap heap(1);
  std::array roots{Value::String(heap.AllocateString("中😀")), Value::Null()};
  RootRange root(heap, roots.data(), roots.size());
  ASSERT_NE(roots.front().AsString(), nullptr);
  const std::array arguments{roots.front(), Value::Integer(0), Value::Integer(3), roots.front()};
  heap.ResetAllocationStatistics();
  const auto result = ExecuteBuiltin(Builtin::StringReplaceBytes, arguments, heap);
  ASSERT_TRUE(result);
  EXPECT_EQ(heap.allocation_statistics().count, 1U);
  roots.back() = *result;
  heap.Collect();
  EXPECT_EQ(roots.back().AsString()->value(), "中😀😀");
  EXPECT_EQ(roots.back().AsString()->length(), 3U);
}

TEST(StringGcTest, ByteReplacementPreservesCodePointCountsAcrossAllBoundaryRanges) {
  constexpr std::string_view kText("A中😀éZ\0", sizeof("A中😀éZ\0") - 1);
  constexpr std::array<std::size_t, 8> kBoundaries{0, 1, 4, 8, 9, 11, 12, 13};
  struct Insertion final {
    std::string_view text;
    std::size_t points;
  };
  constexpr auto kInsertions = std::to_array<Insertion>({
      {.text = "", .points = 0},
      {.text = "ascii", .points = 5},
      {.text = "汉🙂", .points = 2},
      {.text = "é", .points = 2},
      {.text = {"x\0中", sizeof("x\0中") - 1}, .points = 3},
  });
  Heap heap(1);
  std::array roots{Value::Null(), Value::Null(), Value::Null()};
  RootRange root(heap, roots.data(), roots.size());
  roots[0] = Value::String(heap.AllocateString(kText));
  ASSERT_NE(roots[0].AsString(), nullptr);
  for (const auto& insertion : kInsertions) {
    roots[1] = Value::String(heap.AllocateString(insertion.text));
    ASSERT_NE(roots[1].AsString(), nullptr);
    for (std::size_t first_point = 0; first_point < kBoundaries.size(); ++first_point) {
      for (auto last_point = first_point; last_point < kBoundaries.size(); ++last_point) {
        SCOPED_TRACE(testing::Message()
                     << first_point << ", " << last_point << ", " << insertion.text);
        const auto start = kBoundaries[first_point];
        const auto end = kBoundaries[last_point];
        const std::array arguments{
            roots[0],
            Value::Integer(static_cast<std::int64_t>(start)),
            Value::Integer(static_cast<std::int64_t>(end)),
            roots[1],
        };
        const auto result = ExecuteBuiltin(Builtin::StringReplaceBytes, arguments, heap);
        ASSERT_TRUE(result);
        roots[2] = *result;
        heap.Collect();
        std::string expected(kText.substr(0, start));
        expected.append(insertion.text);
        expected.append(kText.substr(end));
        const auto* output = roots[2].AsString();
        const auto points = output->length();
        EXPECT_EQ(output->value(), expected);
        EXPECT_EQ(points, kBoundaries.size() - 1 - (last_point - first_point) + insertion.points);
        EXPECT_EQ(output->ByteOffset(points), expected.size());
      }
    }
  }
}

TEST(StringGcTest, TrimUsesUnicodeWhiteSpaceAndPreservesNulBomAndZeroWidthSpace) {
  Heap heap(1);
  Value value = Value::Null();
  RootRange root(heap, &value, 1);
  for (const auto* const whitespace : {
           "\t\n\v\f\r ",
           "\u0085\u00a0\u1680",
           "\u2000\u2001\u2002\u2003\u2004\u2005",
           "\u2006\u2007\u2008\u2009\u200a",
           "\u2028\u2029\u202f\u205f\u3000",
       }) {
    const std::string padding(whitespace);
    auto text = padding;
    text.append("中 😀");
    text.append(padding);
    value = Value::String(heap.AllocateString(text));
    ASSERT_NE(value.AsString(), nullptr);
    const auto result = ExecuteBuiltin(Builtin::StringTrim, std::span(&value, 1), heap);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->AsString()->value(), "中 😀");
  }
  for (const auto text : {
           std::string_view(""),
           std::string_view("\0　\0", sizeof("\0　\0") - 1),
           std::string_view("\ufeff中\u200b"),
       }) {
    value = Value::String(heap.AllocateString(text));
    ASSERT_NE(value.AsString(), nullptr);
    const auto result = ExecuteBuiltin(Builtin::StringTrim, std::span(&value, 1), heap);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->AsString(), value.AsString());
  }
  value = Value::String(heap.AllocateString("　 \t"));
  const auto empty = ExecuteBuiltin(Builtin::StringTrim, std::span(&value, 1), heap);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->AsString()->value().empty());
  EXPECT_EQ(empty->AsString()->length(), 0U);
}

TEST(StringGcTest, ByteOperationsRejectWrongRuntimeKindsBeforeReadingTheirPayloads) {
  Heap heap;
  const auto text = Value::String(heap.AllocateString("中"));
  for (const auto builtin : {
           Builtin::StringIsCodePointBoundary,
           Builtin::StringSliceBytes,
           Builtin::StringReplaceBytes,
           Builtin::StringByteOffsetAt,
           Builtin::StringTrim,
       }) {
    std::array arguments{Value::Integer(1), Value::Integer(0), Value::Integer(3), text};
    EXPECT_EQ(ExecuteBuiltin(builtin, arguments, heap).error(), ExecutionStatus::TypeError);
    if (builtin == Builtin::StringTrim) {
      continue;
    }
    arguments[0] = text;
    arguments[1] = Value::Boolean(false);
    EXPECT_EQ(ExecuteBuiltin(builtin, arguments, heap).error(), ExecutionStatus::TypeError);
  }
}

TEST(StringGcTest, IndexedCodePointsAppendWithoutManagedCharacterAllocations) {
  Heap heap(1);
  constexpr std::string_view kText("A中😀é\0", sizeof("A中😀é\0") - 1);
  std::array roots{Value::String(heap.AllocateString(kText)), Value::Null()};
  RootRange root(heap, roots.data(), roots.size());
  roots.back() = Value::Object(heap.AllocateMutableString());
  ASSERT_NE(roots.front().AsString(), nullptr);
  ASSERT_NE(roots.back().AsObject(), nullptr);
  constexpr auto kExpected = std::to_array<std::int64_t>({65, 20013, 128512, 101, 769, 0});
  for (std::size_t index = 0; index < kExpected.size(); ++index) {
    const auto count = heap.object_count();
    const std::array indexed{roots.front(), Value::Integer(static_cast<std::int64_t>(index))};
    const auto point = ExecuteBuiltin(Builtin::StringCodePointAt, indexed, heap);
    ASSERT_TRUE(point);
    ASSERT_TRUE(point->IsInteger());
    EXPECT_EQ(point->AsInteger(), kExpected.at(index));
    EXPECT_EQ(heap.object_count(), count);
    const std::array appended{roots.back(), *point};
    ASSERT_TRUE(ExecuteBuiltin(Builtin::MutableStringAppendCodePoint, appended, heap));
  }
  EXPECT_EQ(heap.object_count(), roots.size());
  const auto built = ExecuteBuiltin(Builtin::MutableStringToString, std::span(roots).last(1), heap);
  ASSERT_TRUE(built);
  EXPECT_EQ(built->AsString()->value(), kText);
  for (const auto index : {-1, 6}) {
    const std::array arguments{roots.front(), Value::Integer(index)};
    const auto result = ExecuteBuiltin(Builtin::StringCodePointAt, arguments, heap);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), ExecutionStatus::IndexOutOfBounds);
  }
  for (const auto point : {-1, 0xD800, 0xDFFF, 0x110000}) {
    const std::array arguments{roots.back(), Value::Integer(point)};
    const auto result = ExecuteBuiltin(Builtin::MutableStringAppendCodePoint, arguments, heap);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), ExecutionStatus::TypeError);
  }
}

TEST(StringGcTest, AccountsAndReleasesLargePayload) {
  Heap heap;
  constexpr std::size_t kLength = 8192;
  const std::string text(kLength, 'x');
  Value value = Value::String(heap.AllocateString(text));
  RootRange root(heap, &value, 1);
  ASSERT_NE(value.AsString(), nullptr);
  EXPECT_EQ(heap.allocated_bytes(), sizeof(StringObject) + kLength);
  EXPECT_EQ(heap.committed_bytes(), heap.allocated_bytes());
  heap.Collect();
  EXPECT_EQ(value.AsString()->value(), text);
  value = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  EXPECT_EQ(heap.committed_bytes(), 0U);
}

TEST(StringGcTest, KeepsCodePointBoundariesAcrossForwardReverseAndScatteredAccess) {
  constexpr std::array<std::string_view, 6> kPoints = {"A", "中", "😀", "e", "́", {"\0", 1}};
  constexpr std::size_t kRepetitions = 512;
  constexpr std::size_t kScatterStride = 1543;
  std::string text;
  std::vector<std::size_t> offsets{0};
  for (std::size_t repeat = 0; repeat < kRepetitions; ++repeat) {
    for (const auto point : kPoints) {
      text += point;
      offsets.push_back(text.size());
    }
  }
  Heap heap(1);
  const auto value = Value::String(heap.AllocateString(text));
  RootRange root(heap, &value, 1);
  const auto* string = value.AsString();
  ASSERT_NE(string, nullptr);
  EXPECT_EQ(string->length(), kPoints.size() * kRepetitions);
  for (std::uint32_t index = 0; index < offsets.size(); ++index) {
    EXPECT_EQ(string->ByteOffset(index), offsets.at(index));
  }
  heap.Collect();
  for (auto index = static_cast<std::uint32_t>(offsets.size()); index != 0;) {
    --index;
    EXPECT_EQ(string->ByteOffset(index), offsets.at(index));
  }
  for (std::uint32_t step = 0; step < offsets.size(); ++step) {
    const auto index = static_cast<std::uint32_t>((step * kScatterStride) % offsets.size());
    EXPECT_EQ(string->ByteOffset(index), offsets.at(index));
  }
  EXPECT_EQ(string->value(), text);
  EXPECT_EQ(heap.allocated_bytes(), sizeof(StringObject) + text.size());
}

TEST(StringGcTest, ReusesBlocksAcrossStringAndInstanceLifetimes) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* layout = isolate.CreateClassLayout(0, {});
  ASSERT_NE(heap.AllocateString("vela"), nullptr);
  const auto committed = heap.committed_bytes();
  heap.Collect();
  constexpr int kReuseIterations = 32;
  for (int iteration = 0; iteration < kReuseIterations; ++iteration) {
    ASSERT_NE(heap.AllocateString("vela"), nullptr);
    heap.Collect();
    ASSERT_NE(heap.AllocateInstance(*layout), nullptr);
    heap.Collect();
  }
  EXPECT_EQ(heap.committed_bytes(), committed);
}

TEST(StringGcTest, RootRangeRetainsStringsAndDetaches) {
  Heap heap(1);
  std::array<StringObject*, 3> constants{};
  {
    RootRange roots(heap, constants);
    for (auto& constant : constants) {
      constant = heap.AllocateString("constant");
      ASSERT_NE(constant, nullptr);
    }
    heap.Collect();
    EXPECT_EQ(heap.object_count(), constants.size());
    for (const auto* constant : constants) {
      EXPECT_EQ(constant->value(), "constant");
    }
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(StringGcTest, CollectsCycleContainingStringsAndArrays) {
  Isolate isolate;
  auto& heap = isolate.heap();
  constexpr std::array<std::uint32_t, 2> kReferences = {0, 1};
  auto* layout = isolate.CreateClassLayout(2, kReferences);
  const auto* arrays = isolate.CreateArrayLayout(
      {.element = {.kind = ValueKind::Object, .nominal = &layout->nominal_type()}});
  Value root = Value::Object(heap.AllocateInstance(*layout));
  RootRange roots(heap, &root, 1);
  auto* string = heap.AllocateString("inside cycle");
  ASSERT_TRUE(heap.SetField(*root.AsObject(), 0, Value::String(string)));
  auto* array = heap.AllocateArray(*arrays, 1);
  ASSERT_TRUE(heap.SetField(*root.AsObject(), 1, Value::Object(array)));
  ASSERT_TRUE(heap.SetElement(*array, 0, root));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 3U);
  EXPECT_EQ(Heap::LoadField(*root.AsObject(), 0).AsString()->value(), "inside cycle");
  root = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

class StringBarrierTest : public testing::TestWithParam<std::size_t> {};

TEST_P(StringBarrierTest, ArrayStoreMarksWhiteStringDuringMarkAndSweep) {
  const auto steps = GetParam();
  for (const auto nullable : {false, true}) {
    SCOPED_TRACE(nullable);
    Isolate isolate;
    auto& heap = isolate.heap();
    const auto* layout =
        isolate.CreateArrayLayout({.element = {.kind = ValueKind::String, .nullable = nullable}});
    EXPECT_EQ(layout->element_size(), sizeof(StringObject*));
    auto* string = heap.AllocateString("retained");
    const auto root = Value::Object(heap.AllocateArray(*layout, 1));
    RootRange roots(heap, &root, 1);
    auto& array = *root.AsObject();
    heap.StartCollection();
    heap.Step(steps);
    ASSERT_EQ(heap.phase(), steps == 1 ? GcPhase::Mark : GcPhase::Sweep);
    ASSERT_TRUE(heap.SetElement(array, 0, Value::String(string)));
    heap.FinishCollection();
    ASSERT_EQ(heap.object_count(), 2U);
    const auto retained = heap.GetElement(array, 0);
    ASSERT_TRUE(retained);
    ASSERT_TRUE(retained->IsString());
    ASSERT_EQ(retained->AsString(), string);
    EXPECT_EQ(string->value(), "retained");
    if (nullable) {
      ASSERT_TRUE(heap.SetElement(array, 0, Value::Null()));
    }
    heap.Collect();
    EXPECT_EQ(heap.object_count(), nullable ? 1U : 2U);
  }
}

TEST_P(StringBarrierTest, FieldStoreMarksWhiteStringDuringMarkAndSweep) {
  Isolate isolate;
  auto& heap = isolate.heap();
  constexpr std::array<std::uint32_t, 1> kReferences = {0};
  auto* layout = isolate.CreateClassLayout(1, kReferences);
  auto* string = heap.AllocateString("retained");
  Value root = Value::Object(heap.AllocateInstance(*layout));
  RootRange roots(heap, &root, 1);
  heap.StartCollection();
  heap.Step(GetParam());
  ASSERT_TRUE(heap.SetField(*root.AsObject(), 0, Value::String(string)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(Heap::LoadField(*root.AsObject(), 0).AsString()->value(), "retained");
}

INSTANTIATE_TEST_SUITE_P(IncrementalPhases, StringBarrierTest, testing::Values(1U, 2U));

TEST(StringGcTest, RescansChangedRootBeforeSweep) {
  Heap heap;
  Value value = Value::Null();
  RootRange root(heap, &value, 1);
  auto* string = heap.AllocateString("moved to root");
  heap.StartCollection();
  value = Value::String(string);
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 1U);
  EXPECT_EQ(value.AsString()->value(), "moved to root");
}

TEST(StringGcTest, RegistersNativeRootsAndAllocatesDuringSweep) {
  Heap heap;
  std::array<StringObject*, 1> constants = {heap.AllocateString("unswept")};
  ASSERT_NE(heap.AllocateString("garbage"), nullptr);
  heap.StartCollection();
  heap.Step(1);
  ASSERT_EQ(heap.phase(), GcPhase::Sweep);
  RootRange roots(heap, constants);
  Value value = Value::String(heap.AllocateString("new string"));
  RootRange new_root(heap, &value, 1);
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(constants.front()->value(), "unswept");
  EXPECT_EQ(value.AsString()->value(), "new string");
}

TEST(StringGcTest, BuilderAccountsExternalStorageAndReleasesItOnCollection) {
  Heap heap(1);
  Value value = Value::Object(heap.AllocateMutableString());
  RootRange root(heap, &value, 1);
  ASSERT_NE(value.AsObject(), nullptr);
  const auto initial = heap.allocated_bytes();
  const std::string text(8192, 'x');
  ASSERT_TRUE(heap.AppendString(*value.AsObject(), text));
  EXPECT_EQ(heap.allocated_bytes(), initial + text.size());
  auto* snapshot = heap.BuildString(*value.AsObject());
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->value(), text);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), initial + text.size());
  value = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.committed_bytes(), 0U);
}

TEST(StringGcTest, BuilderGrowthIsGeometricAndSnapshotsOwnIndependentStorage) {
  Heap heap(1);
  std::array<Value, 2> values = {Value::Object(heap.AllocateMutableString()), Value::Null()};
  RootRange roots(heap, values.data(), values.size());
  auto& builder = *values.front().AsObject();
  constexpr std::size_t kAppends = 4096;
  for (std::size_t index = 0; index < kAppends; ++index) {
    ASSERT_TRUE(heap.AppendString(builder, "x"));
  }
  values.back() = Value::String(heap.BuildString(builder));
  ASSERT_NE(values.back().AsString(), nullptr);
  const auto before = heap.allocated_bytes();
  ASSERT_TRUE(heap.AppendString(builder, values.back().AsString()->value()));
  heap.Collect();
  EXPECT_EQ(values.back().AsString()->value(), std::string(kAppends, 'x'));
  EXPECT_EQ(heap.allocated_bytes(), before + kAppends);
}

TEST_P(StringBarrierTest, BuilderSurvivesIncrementalCollectionAndGrowth) {
  Heap heap;
  Value value = Value::Object(heap.AllocateMutableString());
  RootRange root(heap, &value, 1);
  ASSERT_TRUE(heap.AppendString(*value.AsObject(), "old"));
  ASSERT_NE(heap.AllocateString("garbage"), nullptr);
  heap.StartCollection();
  heap.Step(GetParam());
  const std::string suffix(8192, 'x');
  ASSERT_TRUE(heap.AppendString(*value.AsObject(), suffix));
  heap.FinishCollection();
  auto* snapshot = heap.BuildString(*value.AsObject());
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->value(), "old" + suffix);
}

class StringConversionTest : public testing::TestWithParam<Value> {};

TEST_P(StringConversionTest, ExplicitConversionMatchesConsoleFormatting) {
  Heap heap;
  const auto value = GetParam();
  const std::array<Value, 1> arguments = {value};
  const auto converted = ExecuteBuiltin(Builtin::ScalarToString, arguments, heap);
  ASSERT_TRUE(converted.has_value());
  std::ostringstream output;
  WriteValue(output, value);
  EXPECT_EQ(converted->AsString()->value(), output.str());
  const auto length = ExecuteBuiltin(Builtin::ScalarTextLength, arguments, heap);
  ASSERT_TRUE(length);
  EXPECT_EQ(length->AsInteger(), output.str().size());
  const auto joined = heap.JoinValues(arguments);
  ASSERT_TRUE(joined.has_value());
  EXPECT_EQ((*joined)->value(), output.str());
}

INSTANTIATE_TEST_SUITE_P(PrimitiveValues,
                         StringConversionTest,
                         testing::Values(Value::Integer(std::numeric_limits<std::int64_t>::min()),
                                         Value::Integer(std::numeric_limits<std::int64_t>::max()),
                                         Value::Boolean(true),
                                         Value::Boolean(false),
                                         Value::Double(-0.0),
                                         Value::Double(kRoundTripValue),
                                         Value::Double(std::numeric_limits<double>::infinity()),
                                         Value::Double(std::numeric_limits<double>::quiet_NaN()),
                                         Value::Double(std::numeric_limits<double>::denorm_min())));

}  // namespace
}  // namespace vela::runtime
