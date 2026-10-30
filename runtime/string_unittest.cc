#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "base/builtin.h"
#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "runtime/value_io.h"

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

TEST(StringGcTest, NativeConstantRootsRetainStringsAndDetach) {
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
  EXPECT_EQ(Heap::GetField(*root.AsObject(), 0)->AsString()->value(), "inside cycle");
  root = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

class StringBarrierTest : public testing::TestWithParam<std::size_t> {};

TEST_P(StringBarrierTest, ArrayStoreMarksWhiteStringDuringMarkAndSweep) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateArrayLayout({.element = {.kind = ValueKind::String}});
  auto* string = heap.AllocateString("retained");
  Value root = Value::Object(heap.AllocateArray(*layout, 1));
  RootRange roots(heap, &root, 1);
  heap.StartCollection();
  heap.Step(GetParam());
  EXPECT_EQ(heap.phase(), GetParam() == 1 ? GcPhase::Mark : GcPhase::Sweep);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::String(string)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(Heap::GetElement(*root.AsObject(), 0)->AsString()->value(), "retained");
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
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
  EXPECT_EQ(Heap::GetField(*root.AsObject(), 0)->AsString()->value(), "retained");
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
  EXPECT_EQ(heap.committed_bytes(), initial);
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
