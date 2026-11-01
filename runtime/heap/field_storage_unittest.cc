#include "runtime/heap/field_storage.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(FieldStorageTest, CompactSlotsPreservePayloadBitsAndIndependentKinds) {
  const std::array values{
      Value::Null(),
      Value::Boolean(true),
      Value::Boolean(false),
      Value::Integer(std::numeric_limits<std::int64_t>::min()),
      Value::Integer(std::numeric_limits<std::int64_t>::max()),
      Value::Double(-0.0),
      Value::Double(std::bit_cast<double>(std::uint64_t{0x7ff8000000000042})),
  };
  constexpr auto kCount = static_cast<std::uint32_t>(values.size());
  std::array<std::byte, FieldStorage::Size(kCount)> storage{};
  EXPECT_LT(storage.size(), sizeof(Value) * values.size());
  for (std::uint32_t index = 0; index < kCount; ++index) {
    FieldStorage::Store(storage, kCount, index, values.at(index));
  }
  for (std::uint32_t index = 0; index < kCount; ++index) {
    const auto expected = values.at(index);
    const auto value = FieldStorage::Load(storage, kCount, index);
    EXPECT_EQ(value.kind(), expected.kind());
    if (value.IsDouble()) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(value.AsDouble()),
                std::bit_cast<std::uint64_t>(expected.AsDouble()));
    } else if (value.IsInteger()) {
      EXPECT_EQ(value.AsInteger(), expected.AsInteger());
    } else if (value.IsBoolean()) {
      EXPECT_EQ(value.AsBoolean(), expected.AsBoolean());
    }
  }
  for (std::uint32_t index = 0; index < kCount; ++index) {
    FieldStorage::Store(storage, kCount, index, Value::Null());
    EXPECT_TRUE(FieldStorage::Load(storage, kCount, index).IsNull());
    if (index + 1 < kCount) {
      EXPECT_EQ(FieldStorage::Load(storage, kCount, index + 1).kind(), values.at(index + 1).kind());
    }
  }
}

}  // namespace
}  // namespace vela::runtime
