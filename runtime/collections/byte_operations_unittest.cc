#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

#include "gtest/gtest.h"

#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/execution_status.h"
#include "runtime/collections/byte_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

constexpr std::size_t kOwnerLength = 24;
constexpr std::uint32_t kViewLength = 16;
constexpr std::uint8_t kSentinel = 7;
constexpr std::uint8_t kAnswer = 42;
constexpr double kNegativeOneAndHalf = -1.5;
constexpr double kEvenHalfway = 0x1.000001p0;
constexpr double kOddHalfway = 0x1.000003p0;
constexpr double kFloatMinimumSubnormal = 0x1p-149;
constexpr double kUnderflowHalfway = 0x1p-150;
constexpr double kSubnormalHalfway = 0x1.8p-149;
constexpr double kFloatMinimumNormal = 0x1p-126;
constexpr double kNormalBoundaryHalfway = 0x1.fffffep-127;
constexpr double kOverflowHalfway = 0x1.ffffffp127;

class ByteOperationsTest : public testing::Test {
 protected:
  void SetUp() override {
    roots().front() = Value::Object(heap().AllocateByteArray(kOwnerLength));
    ASSERT_NE(roots().front().AsObject(), nullptr);
    roots().at(1) = Value::Object(
        heap().AllocateByteData(*roots().front().AsObject(), {.offset = 1, .length = kViewLength}));
    ASSERT_NE(roots().at(1).AsObject(), nullptr);
  }

  std::expected<Value, ExecutionStatus> Call(Builtin builtin, std::initializer_list<Value> values) {
    return ExecuteByteBuiltin(builtin, values, heap());
  }

  Heap& heap() { return heap_; }
  std::array<Value, 3>& roots() { return roots_; }

 private:
  Heap heap_{1};
  std::array<Value, 3> roots_{};
  RootRange root_{heap_, roots_.data(), roots_.size()};
};

TEST_F(ByteOperationsTest, ZeroInitializesFixedStorageAndRejectsUnrepresentableLengths) {
  const auto bytes = Heap::ByteArrayBytes(*roots().front().AsObject());
  EXPECT_TRUE(std::ranges::all_of(bytes, [](auto byte) { return byte == 0; }));
  const auto empty = Call(Builtin::NewByteArray, {Value::Integer(0)});
  ASSERT_TRUE(empty);
  EXPECT_TRUE(Heap::ByteArrayBytes(*empty->AsObject()).empty());
  EXPECT_EQ(Call(Builtin::NewByteArray, {Value::Integer(-1)}).error(),
            ExecutionStatus::InvalidNumericConversion);
  EXPECT_EQ(Call(Builtin::NewByteArray, {Value::Integer(std::numeric_limits<std::int64_t>::max())})
                .error(),
            ExecutionStatus::OutOfMemory);
  EXPECT_EQ(Call(Builtin::NewByteArray, {Value::Double(8.0)}).error(), ExecutionStatus::TypeError);
}

TEST_F(ByteOperationsTest, ByteWritesCheckValueAndIndexBeforeMutating) {
  auto bytes = Heap::ByteArrayBytes(*roots().front().AsObject());
  std::ranges::fill(bytes, kSentinel);
  EXPECT_EQ(
      Call(Builtin::ByteArraySetIndex, {roots().front(), Value::Integer(0), Value::Integer(256)})
          .error(),
      ExecutionStatus::InvalidNumericConversion);
  EXPECT_EQ(
      Call(Builtin::ByteArraySetIndex, {roots().front(), Value::Integer(0), Value::Integer(-1)})
          .error(),
      ExecutionStatus::InvalidNumericConversion);
  EXPECT_TRUE(std::ranges::all_of(bytes, [](auto byte) { return byte == kSentinel; }));
  EXPECT_EQ(Call(Builtin::ByteArrayGetIndex, {roots().front(), Value::Integer(-1)}).error(),
            ExecutionStatus::IndexOutOfBounds);
  EXPECT_EQ(Call(Builtin::ByteArrayGetIndex,
                 {roots().front(), Value::Integer(std::numeric_limits<std::int64_t>::max())})
                .error(),
            ExecutionStatus::IndexOutOfBounds);
  ASSERT_TRUE(
      Call(Builtin::ByteArraySetIndex, {roots().front(), Value::Integer(23), Value::Integer(255)}));
  EXPECT_EQ(bytes.back(), 255);
}

TEST_F(ByteOperationsTest, CopiesOnlyValidatedIntArraysAndDoesNotShareStorage) {
  const ArrayLayout layout{.element = {.kind = ValueKind::Integer}};
  roots().at(2) = Value::Object(heap().AllocateArray(layout, 3));
  auto& array = *roots().at(2).AsObject();
  ASSERT_TRUE(heap().SetElement(array, 0, Value::Integer(42)));
  ASSERT_TRUE(heap().SetElement(array, 1, Value::Integer(255)));
  ASSERT_TRUE(heap().SetElement(array, 2, Value::Integer(256)));
  EXPECT_EQ(Call(Builtin::ByteArrayFrom, {roots().at(2)}).error(),
            ExecutionStatus::InvalidNumericConversion);
  ASSERT_TRUE(heap().SetElement(array, 2, Value::Integer(0)));
  const auto copy = Call(Builtin::ByteArrayFrom, {roots().at(2)});
  ASSERT_TRUE(copy);
  const auto bytes = Heap::ByteArrayBytes(*copy->AsObject());
  EXPECT_EQ(bytes.front(), 42);
  EXPECT_EQ(bytes.subspan(1).front(), 255);
  bytes.front() = 0;
  EXPECT_EQ(Heap::GetElement(array, 0)->AsInteger(), 42);
  EXPECT_EQ(Call(Builtin::ByteArrayFrom, {roots().front()}).error(), ExecutionStatus::TypeError);
}

struct SliceSample final {
  std::int64_t start;
  std::int64_t end;
  std::vector<std::uint8_t> expected;
};

class ByteSliceTest : public ByteOperationsTest, public testing::WithParamInterface<SliceSample> {};

TEST_P(ByteSliceTest, NormalizesEndpointsAndReturnsIndependentStorage) {
  const auto owner = roots().front();
  auto bytes = Heap::ByteArrayBytes(*owner.AsObject());
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes.subspan(index).front() = static_cast<std::uint8_t>(index);
  }
  const auto& sample = GetParam();
  const std::vector<std::uint8_t> original(bytes.begin(), bytes.end());
  const auto result = Call(Builtin::ByteArraySlice,
                           {owner, Value::Integer(sample.start), Value::Integer(sample.end)});
  ASSERT_TRUE(result);
  const auto copy = Heap::ByteArrayBytes(*result->AsObject());
  EXPECT_TRUE(std::ranges::equal(copy, sample.expected));
  if (!copy.empty()) {
    copy.front() = static_cast<std::uint8_t>(kMaximumByte);
    EXPECT_TRUE(std::ranges::equal(bytes, original));
  }
}

INSTANTIATE_TEST_SUITE_P(
    EndpointRules,
    ByteSliceTest,
    testing::Values(SliceSample{.start = -3, .end = -1, .expected = {21, 22}},
                    SliceSample{.start = 9, .end = 2, .expected = {}},
                    SliceSample{.start = 100, .end = 200, .expected = {}},
                    SliceSample{.start = std::numeric_limits<std::int64_t>::min(),
                                .end = 2,
                                .expected = {0, 1}},
                    SliceSample{.start = 22,
                                .end = std::numeric_limits<std::int64_t>::max(),
                                .expected = {22, 23}}));

TEST_F(ByteOperationsTest, ViewsShareStorageAndCheckTheirOwnStrictBounds) {
  const auto owner = roots().front();
  const auto view = roots().at(1);
  const auto other = Call(Builtin::ByteDataView, {owner, Value::Integer(2), Value::Null()});
  ASSERT_TRUE(other);
  Heap::ByteDataBytes(*other->AsObject()).front() = kAnswer;
  EXPECT_EQ(Heap::ByteDataBytes(*view.AsObject()).subspan(1).front(), 42);
  EXPECT_EQ(Heap::ByteArrayBytes(*owner.AsObject()).subspan(2).front(), 42);
  EXPECT_EQ(Call(Builtin::ByteDataView, {owner, Value::Integer(-1), Value::Null()}).error(),
            ExecutionStatus::IndexOutOfBounds);
  EXPECT_EQ(Call(Builtin::ByteDataView, {owner, Value::Integer(25), Value::Integer(0)}).error(),
            ExecutionStatus::IndexOutOfBounds);
  EXPECT_EQ(Call(Builtin::ByteDataView, {owner, Value::Integer(1), Value::Integer(24)}).error(),
            ExecutionStatus::IndexOutOfBounds);
  EXPECT_EQ(Call(Builtin::ByteDataView, {owner, Value::Integer(0), Value::Integer(-1)}).error(),
            ExecutionStatus::IndexOutOfBounds);
  const auto empty = Call(Builtin::ByteDataView, {owner, Value::Integer(24), Value::Integer(0)});
  ASSERT_TRUE(empty);
  EXPECT_EQ(Call(Builtin::ByteDataGetUint8, {*empty, Value::Integer(0)}).error(),
            ExecutionStatus::IndexOutOfBounds);
}

struct IntegerSample final {
  Builtin get;
  Builtin set;
  std::size_t width;
  std::int64_t minimum;
  std::int64_t maximum;
};

class ByteIntegerTest : public ByteOperationsTest,
                        public testing::WithParamInterface<IntegerSample> {
 protected:
  void CheckRoundTrip(std::int64_t value, ByteOrder order) {
    const auto& sample = GetParam();
    const auto view = roots().at(1);
    const auto code = Value::Integer(static_cast<std::int64_t>(order));
    const std::array arguments{view, Value::Integer(1), Value::Integer(value), code};
    const auto written = ExecuteByteBuiltin(
        sample.set, std::span(arguments).first(BuiltinArity(sample.set)), heap());
    ASSERT_TRUE(written);
    const std::array reading{view, Value::Integer(1), code};
    const auto read =
        ExecuteByteBuiltin(sample.get, std::span(reading).first(BuiltinArity(sample.get)), heap());
    ASSERT_TRUE(read);
    EXPECT_EQ(read->AsInteger(), value);
  }
};

TEST_P(ByteIntegerTest, PreservesEndpointsInEveryByteOrder) {
  const auto& sample = GetParam();
  for (const auto order : {ByteOrder::Big, ByteOrder::Little, ByteOrder::Host}) {
    CheckRoundTrip(sample.minimum, order);
    CheckRoundTrip(sample.maximum, order);
  }
}

TEST_P(ByteIntegerTest, RejectsOutOfViewOffsetsBeforeWriting) {
  const auto& sample = GetParam();
  const auto view = roots().at(1);
  auto storage = Heap::ByteDataBytes(*view.AsObject());
  std::ranges::fill(storage, kSentinel);
  for (const auto offset : {
           std::int64_t{-1},
           static_cast<std::int64_t>(kViewLength),
           std::numeric_limits<std::int64_t>::max(),
       }) {
    const std::array args{view, Value::Integer(offset), Value::Integer(0), Value::Integer(0)};
    EXPECT_EQ(
        ExecuteByteBuiltin(sample.set, std::span(args).first(BuiltinArity(sample.set)), heap())
            .error(),
        ExecutionStatus::IndexOutOfBounds);
  }
  EXPECT_TRUE(std::ranges::all_of(storage, [](auto byte) { return byte == kSentinel; }));
}

TEST_P(ByteIntegerTest, WritesTheGoldenByteWidthAtTheLastValidOffset) {
  const auto& sample = GetParam();
  const auto view = roots().at(1);
  auto bytes = Heap::ByteDataBytes(*view.AsObject());
  std::ranges::fill(bytes, kSentinel);
  const auto offset = bytes.size() - sample.width;
  const std::array arguments{
      view,
      Value::Integer(static_cast<std::int64_t>(offset)),
      Value::Integer(sample.maximum),
      Value::Integer(0),
  };
  ASSERT_TRUE(
      ExecuteByteBuiltin(sample.set, std::span(arguments).first(BuiltinArity(sample.set)), heap()));
  EXPECT_TRUE(
      std::ranges::all_of(bytes.first(offset), [](auto byte) { return byte == kSentinel; }));
  const auto written = bytes.subspan(offset);
  EXPECT_EQ(written.front(), sample.minimum < 0 ? 0x7f : 0xff);
  EXPECT_TRUE(
      std::ranges::all_of(written.subspan(1), [](auto byte) { return byte == kMaximumByte; }));
}

TEST_P(ByteIntegerTest, RejectsIntegerOverflowBeforeWriting) {
  const auto& sample = GetParam();
  if (sample.width == sizeof(std::int64_t)) {
    return;
  }
  const auto view = roots().at(1);
  auto storage = Heap::ByteDataBytes(*view.AsObject());
  std::ranges::fill(storage, kSentinel);
  for (const auto invalid : {sample.minimum - 1, sample.maximum + 1}) {
    const std::array args{view, Value::Integer(0), Value::Integer(invalid), Value::Integer(0)};
    EXPECT_EQ(
        ExecuteByteBuiltin(sample.set, std::span(args).first(BuiltinArity(sample.set)), heap())
            .error(),
        ExecutionStatus::InvalidNumericConversion);
  }
  EXPECT_TRUE(std::ranges::all_of(storage, [](auto byte) { return byte == kSentinel; }));
}

INSTANTIATE_TEST_SUITE_P(EveryIntegerWidth,
                         ByteIntegerTest,
                         testing::Values(IntegerSample{.get = Builtin::ByteDataGetInt8,
                                                       .set = Builtin::ByteDataSetInt8,
                                                       .width = 1,
                                                       .minimum = -128,
                                                       .maximum = 127},
                                         IntegerSample{.get = Builtin::ByteDataGetUint8,
                                                       .set = Builtin::ByteDataSetUint8,
                                                       .width = 1,
                                                       .minimum = 0,
                                                       .maximum = 255},
                                         IntegerSample{.get = Builtin::ByteDataGetInt16,
                                                       .set = Builtin::ByteDataSetInt16,
                                                       .width = 2,
                                                       .minimum = -32768,
                                                       .maximum = 32767},
                                         IntegerSample{.get = Builtin::ByteDataGetUint16,
                                                       .set = Builtin::ByteDataSetUint16,
                                                       .width = 2,
                                                       .minimum = 0,
                                                       .maximum = 65535},
                                         IntegerSample{.get = Builtin::ByteDataGetInt32,
                                                       .set = Builtin::ByteDataSetInt32,
                                                       .width = 4,
                                                       .minimum = -2147483648LL,
                                                       .maximum = 2147483647},
                                         IntegerSample{.get = Builtin::ByteDataGetUint32,
                                                       .set = Builtin::ByteDataSetUint32,
                                                       .width = 4,
                                                       .minimum = 0,
                                                       .maximum = 4294967295LL},
                                         IntegerSample{
                                             .get = Builtin::ByteDataGetInt64,
                                             .set = Builtin::ByteDataSetInt64,
                                             .width = 8,
                                             .minimum = std::numeric_limits<std::int64_t>::min(),
                                             .maximum = std::numeric_limits<std::int64_t>::max()}));

TEST_F(ByteOperationsTest, EndianGoldenVectorsAndUnalignedViews) {
  const auto view = roots().at(1);
  ASSERT_TRUE(Call(Builtin::ByteDataSetUint32,
                   {view, Value::Integer(0), Value::Integer(0x12345678), Value::Integer(0)}));
  const auto bytes = Heap::ByteArrayBytes(*roots().front().AsObject());
  constexpr auto big = std::to_array<std::uint8_t>({0x12, 0x34, 0x56, 0x78});
  EXPECT_TRUE(std::ranges::equal(bytes.subspan(1, 4), big));
  EXPECT_EQ(
      Call(Builtin::ByteDataGetUint32, {view, Value::Integer(0), Value::Integer(1)})->AsInteger(),
      0x78563412);
  EXPECT_EQ(
      Call(Builtin::ByteDataGetUint32, {view, Value::Integer(0), Value::Integer(2)})->AsInteger(),
      std::endian::native == std::endian::little ? 0x78563412 : 0x12345678);
  EXPECT_EQ(Call(Builtin::ByteDataSetUint32,
                 {view, Value::Integer(0), Value::Integer(0), Value::Integer(3)})
                .error(),
            ExecutionStatus::TypeError);
  EXPECT_TRUE(std::ranges::equal(bytes.subspan(1, 4), big));
}

struct FloatSample final {
  double value;
  std::uint32_t bits;
};

class ByteFloatTest : public ByteOperationsTest, public testing::WithParamInterface<FloatSample> {};

TEST_P(ByteFloatTest, RoundsFloat32ToNearestEvenAndPreservesFloat64) {
  const auto& sample = GetParam();
  const auto view = roots().at(1);
  ASSERT_TRUE(Call(Builtin::ByteDataSetFloat32,
                   {view, Value::Integer(1), Value::Double(sample.value), Value::Integer(0)}));
  const auto raw = Call(Builtin::ByteDataGetUint32, {view, Value::Integer(1), Value::Integer(0)});
  ASSERT_TRUE(raw);
  EXPECT_EQ(raw->AsInteger(), sample.bits);
  const auto read = Call(Builtin::ByteDataGetFloat32, {view, Value::Integer(1), Value::Integer(0)});
  ASSERT_TRUE(read);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(read->AsDouble()),
            std::bit_cast<std::uint64_t>(static_cast<double>(std::bit_cast<float>(sample.bits))));
  ASSERT_TRUE(Call(Builtin::ByteDataSetFloat64,
                   {view, Value::Integer(1), Value::Double(sample.value), Value::Integer(1)}));
  const auto wide = Call(Builtin::ByteDataGetFloat64, {view, Value::Integer(1), Value::Integer(1)});
  ASSERT_TRUE(wide);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(wide->AsDouble()),
            std::bit_cast<std::uint64_t>(sample.value));
}

INSTANTIATE_TEST_SUITE_P(
    IEEEReferenceVectors,
    ByteFloatTest,
    testing::Values(
        FloatSample{.value = 0.0, .bits = 0x00000000},
        FloatSample{.value = -0.0, .bits = 0x80000000},
        FloatSample{.value = 1.0, .bits = 0x3f800000},
        FloatSample{.value = kNegativeOneAndHalf, .bits = 0xbfc00000},
        FloatSample{.value = kEvenHalfway, .bits = 0x3f800000},
        FloatSample{.value = kOddHalfway, .bits = 0x3f800002},
        FloatSample{.value = kFloatMinimumSubnormal, .bits = 0x00000001},
        FloatSample{.value = kUnderflowHalfway, .bits = 0x00000000},
        FloatSample{.value = kSubnormalHalfway, .bits = 0x00000002},
        FloatSample{.value = kFloatMinimumNormal, .bits = 0x00800000},
        FloatSample{.value = kNormalBoundaryHalfway, .bits = 0x00800000},
        FloatSample{.value = kOverflowHalfway, .bits = 0x7f800000},
        FloatSample{.value = std::numeric_limits<double>::max(), .bits = 0x7f800000},
        FloatSample{.value = std::numeric_limits<double>::denorm_min(), .bits = 0x00000000},
        FloatSample{.value = -std::numeric_limits<double>::infinity(), .bits = 0xff800000}));

TEST_F(ByteOperationsTest, AcceptsNanAndDoesNotChangeBytesForFloatTypeOrRangeErrors) {
  const auto view = roots().at(1);
  ASSERT_TRUE(Call(Builtin::ByteDataSetFloat32,
                   {view, Value::Integer(0),
                    Value::Double(std::numeric_limits<double>::quiet_NaN()), Value::Integer(0)}));
  EXPECT_TRUE(std::isnan(
      Call(Builtin::ByteDataGetFloat32, {view, Value::Integer(0), Value::Integer(0)})->AsDouble()));
  auto bytes = Heap::ByteDataBytes(*view.AsObject());
  std::ranges::fill(bytes, kSentinel);
  EXPECT_EQ(Call(Builtin::ByteDataSetFloat64,
                 {view, Value::Integer(0), Value::Integer(1), Value::Integer(0)})
                .error(),
            ExecutionStatus::TypeError);
  EXPECT_EQ(Call(Builtin::ByteDataSetFloat64,
                 {view, Value::Integer(9), Value::Double(0.0), Value::Integer(0)})
                .error(),
            ExecutionStatus::IndexOutOfBounds);
  EXPECT_TRUE(std::ranges::all_of(bytes, [](auto byte) { return byte == kSentinel; }));
}

class RoundingRestore final {
 public:
  RoundingRestore() : previous_(std::fegetround()) {}
  ~RoundingRestore() { static_cast<void>(std::fesetround(previous_)); }

  RoundingRestore(const RoundingRestore&) = delete;
  RoundingRestore& operator=(const RoundingRestore&) = delete;
  RoundingRestore(RoundingRestore&&) = delete;
  RoundingRestore& operator=(RoundingRestore&&) = delete;

 private:
  int previous_;
};

TEST_F(ByteOperationsTest, Float32RoundingIsIndependentOfTheHostRoundingMode) {
  const RoundingRestore restore;
  const auto view = roots().at(1);
  for (const auto mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    ASSERT_EQ(std::fesetround(mode), 0);
    ASSERT_TRUE(Call(Builtin::ByteDataSetFloat32,
                     {view, Value::Integer(0), Value::Double(kEvenHalfway), Value::Integer(0)}));
    EXPECT_EQ(
        Call(Builtin::ByteDataGetUint32, {view, Value::Integer(0), Value::Integer(0)})->AsInteger(),
        0x3f800000);
    EXPECT_EQ(std::fegetround(), mode);
  }
}

TEST_F(ByteOperationsTest, AViewAloneRetainsItsOwnerAndReleasesAllStorageAfterward) {
  const auto baseline = heap().object_count();
  roots().front() = Value::Null();
  heap().Collect();
  EXPECT_EQ(heap().object_count(), baseline);
  Heap::ByteDataBytes(*roots().at(1).AsObject()).front() = kAnswer;
  roots().at(1) = Value::Null();
  heap().FinishCollection();
  heap().Collect();
  EXPECT_EQ(heap().object_count(), 0);
  EXPECT_EQ(heap().allocated_bytes(), 0);
}

TEST_F(ByteOperationsTest, AnEmptyViewRetainsTheEntireOwner) {
  roots().at(1) = Value::Object(
      heap().AllocateByteData(*roots().front().AsObject(), {.offset = kOwnerLength, .length = 0}));
  heap().FinishCollection();
  heap().Collect();
  const auto live = heap().allocated_bytes();
  roots().front() = Value::Null();
  heap().FinishCollection();
  heap().Collect();
  EXPECT_EQ(heap().allocated_bytes(), live);
  EXPECT_TRUE(Heap::ByteDataBytes(*roots().at(1).AsObject()).empty());
  roots().at(1) = Value::Null();
  heap().Collect();
  EXPECT_EQ(heap().allocated_bytes(), 0);
}

}  // namespace
}  // namespace vela::runtime
