#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "base/binary.h"
#include "base/fingerprint.h"
#include "runtime/isolate.h"
#include "vm/bytecode.h"
#include "vm/bytecode_image.h"

namespace vela::vm {
namespace {

struct ImageFunction final {
  Instruction instruction;
  std::uint16_t registers;
  std::uint64_t pool;
};

auto Image(std::span<const std::byte> payload) -> std::vector<std::byte> {
  BinaryWriter writer;
  constexpr std::string_view kMagic = "VELABC01";
  writer.Write(std::as_bytes(std::span(kMagic)));
  writer.Write(kBytecodeImageVersion);
  const auto checksum = ContentFingerprint(payload);
  writer.Write(checksum.low);
  writer.Write(checksum.high);
  writer.Write(payload);
  return std::move(writer).Take();
}

auto FunctionImage(ImageFunction input) -> std::vector<std::byte> {
  BinaryWriter body;
  constexpr std::size_t kSharedTableCount = 6;
  constexpr std::size_t kEmptyFunctionTailCount = 6;
  // 六个空的布局/共享池表；随后写一个最小函数，故意绕过 Program::Create 构造边界。
  for (std::size_t index = 0; index < kSharedTableCount; ++index) {
    body.Write(std::uint64_t{0});
  }
  body.Write(std::uint64_t{1});
  body.Write(std::string_view("main"));
  body.Write(input.pool);
  body.Write(std::numeric_limits<std::uint64_t>::max());
  body.Write(std::numeric_limits<std::uint64_t>::max());
  body.Write(input.registers);
  body.Write(std::uint16_t{0});
  body.Write(std::uint64_t{1});
  body.Write(input.instruction);
  // 整数、浮点、源码映射、异常表、原生符号文本和全局初始化表均为空。
  for (std::size_t index = 0; index < kEmptyFunctionTailCount; ++index) {
    body.Write(std::uint64_t{0});
  }
  return Image(body.bytes());
}

class BytecodeImageBoundaryTest : public testing::TestWithParam<ImageFunction> {};

TEST_P(BytecodeImageBoundaryTest, RejectsChecksummedButInvalidFunctionBeforeExecution) {
  const auto& input = GetParam();
  runtime::Isolate isolate;
  const auto image = FunctionImage(input);
  EXPECT_FALSE(LoadBytecode(image, isolate));
}

constexpr auto kNoPool = std::numeric_limits<std::uint64_t>::max();
INSTANTIATE_TEST_SUITE_P(
    InvalidMetadataAndInstructions,
    BytecodeImageBoundaryTest,
    testing::Values(ImageFunction{EncodeABC(static_cast<OpCode>(0xff), 0, 0, 0), 1, kNoPool},
                    ImageFunction{EncodeABC(OpCode::Return, 1, 0, 0), 1, kNoPool},
                    ImageFunction{EncodeABC(OpCode::Return, 0, 0, 0), 0, kNoPool},
                    ImageFunction{EncodeABC(OpCode::Return, 0, 0, 0), 1, 0},
                    ImageFunction{EncodeABC(OpCode::LoadNull, 0, 0, 0), 1, kNoPool},
                    ImageFunction{EncodeAImm16(OpCode::Jump, 0, 10), 1, kNoPool}));

TEST(BytecodeImageBoundaryTest, LoadsMinimalValidImageAndRejectsExtraDataAndHugeCount) {
  runtime::Isolate isolate;
  EXPECT_TRUE(LoadBytecode(
      FunctionImage(
          {.instruction = EncodeABC(OpCode::Return, 0, 0, 0), .registers = 1, .pool = kNoPool}),
      isolate));
  BinaryWriter body;
  body.Write(std::numeric_limits<std::uint64_t>::max());
  EXPECT_FALSE(LoadBytecode(Image(body.bytes()), isolate));
  const auto trailing = std::to_array<std::byte>({std::byte{0xff}});
  auto valid = FunctionImage(
      {.instruction = EncodeABC(OpCode::Return, 0, 0, 0), .registers = 1, .pool = kNoPool});
  // 重签名额外 payload，确保测试进入结构校验而非只命中 checksum。
  constexpr std::size_t kHeaderBytes = 8 + sizeof(std::uint32_t) + (2 * sizeof(std::uint64_t));
  BinaryWriter changed;
  changed.Write(std::span(valid).subspan(kHeaderBytes));
  changed.Write(trailing);
  EXPECT_FALSE(LoadBytecode(Image(changed.bytes()), isolate));
}

TEST(BytecodeImageBoundaryTest, FailedLoadingReleasesPartiallyCreatedLayoutMetadata) {
  runtime::Isolate isolate;
  const auto initial = isolate.layout_counts();
  BinaryWriter body;
  body.Write(std::uint64_t{1});
  body.Write(std::uint8_t{0});
  body.Write(std::uint32_t{0});
  body.Write(std::uint64_t{0});
  body.Write(std::uint8_t{0});
  body.Write(std::uint64_t{0});
  body.Write(std::uint64_t{0});
  // 完整类记录后缺少数组表；进入重建并分配布局后才失败。
  EXPECT_FALSE(LoadBytecode(Image(body.bytes()), isolate));
  EXPECT_EQ(isolate.layout_counts(), initial);
}

}  // namespace
}  // namespace vela::vm
