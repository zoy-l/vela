#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "base/binary.h"

namespace vela {
namespace {

TEST(BinaryTest, RoundTripsFixedWidthsRecordsAndUtf8) {
  BinaryWriter writer;
  constexpr std::uint64_t kLarge = 0x1234567890abcdef;
  constexpr std::uint16_t kMedium = 0xabcd;
  writer.Write(kLarge);
  writer.Write(kMedium);
  writer.Write(std::string_view("你好"));
  BinaryReader reader(writer.bytes());
  const auto record = reader.Record<std::uint64_t, std::uint16_t>();
  ASSERT_TRUE(record);
  EXPECT_EQ(std::get<0>(*record), kLarge);
  EXPECT_EQ(std::get<1>(*record), kMedium);
  EXPECT_EQ(reader.Text(), "你好");
  EXPECT_EQ(reader.remaining(), 0U);
  EXPECT_FALSE(reader.Read<std::uint8_t>());
}

TEST(BinaryTest, RejectsEveryTruncationAndCountsBeforeAllocation) {
  BinaryWriter writer;
  constexpr std::uint32_t kLength = 20;
  writer.Write(kLength);
  for (std::size_t count = 0; count < writer.bytes().size(); ++count) {
    BinaryReader reader(writer.bytes().first(count));
    EXPECT_FALSE(reader.Read<std::uint32_t>());
  }
  BinaryReader reader(writer.bytes());
  EXPECT_FALSE(reader.Count(sizeof(std::uint32_t)));
  BinaryReader text(writer.bytes());
  EXPECT_FALSE(text.Text());
}

TEST(BinaryTest, ReadsTheSameCountWidthAsWriters) {
  BinaryWriter writer;
  writer.Write(std::uint64_t{1});
  constexpr std::uint32_t kValue = 42;
  writer.Write(kValue);
  BinaryReader reader(writer.bytes());
  EXPECT_EQ(reader.Count(sizeof(std::uint32_t)), 1U);
  EXPECT_EQ(reader.Read<std::uint32_t>(), 42U);
  EXPECT_EQ(reader.remaining(), 0U);
}

}  // namespace
}  // namespace vela
