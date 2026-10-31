#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "base/async_operation.h"
#include "base/binary.h"
#include "base/builtin.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "vm/bytecode.h"
#include "vm/bytecode_image.h"
#include "vm/bytecode_image_internal.h"

namespace vela::vm {
namespace {

struct ImageFunction final {
  Instruction instruction;
  std::uint16_t registers;
  std::uint64_t pool;
};

std::vector<std::byte> Image(std::span<const std::byte> payload) {
  BinaryWriter writer;
  constexpr std::string_view kMagic = "VELABC01";
  writer.Write(std::as_bytes(std::span(kMagic)));
  writer.Write(kBytecodeImageVersion);
  writer.WriteChecksummed(payload);
  return std::move(writer).Take();
}

std::vector<std::byte> FunctionImage(ImageFunction input) {
  BinaryWriter body;
  constexpr std::size_t kSharedTableCount = 8;
  constexpr std::size_t kEmptyFunctionTailCount = 6;
  // 空的布局/共享池表；随后写一个最小函数，故意绕过 Program::Create 构造边界。
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
  body.Write(std::uint8_t{0});
  body.Write(std::uint8_t{0});
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

void WriteElement(BinaryWriter& writer, const internal::ImageElement& type) {
  writer.Write(type.kind);
  writer.Write(type.object_kind);
  writer.Write(type.flags);
  writer.Write(type.interface);
  writer.Write(type.instance);
  writer.Write(type.array);
  writer.Write(type.collection);
  writer.Write(type.signature);
}

std::vector<std::byte> ImageWithLayouts(BinaryWriter& body) {
  constexpr std::size_t kRemainingPools = 3;
  for (std::size_t index = 0; index < kRemainingPools; ++index) {
    body.Write(std::uint64_t{0});
  }
  const auto minimal = FunctionImage({
      .instruction = EncodeABC(OpCode::Return, 0, 0, 0),
      .registers = 1,
      .pool = kNoPool,
  });
  constexpr std::size_t kHeaderBytes =
      internal::kImageMagic.size() + sizeof(std::uint32_t) + (2 * sizeof(std::uint64_t));
  constexpr std::size_t kEmptyPools = 8;
  body.Write(std::span(minimal).subspan(kHeaderBytes + (kEmptyPools * sizeof(std::uint64_t))));
  return Image(body.bytes());
}

TEST(BytecodeImageBoundaryTest, RebuildsDeepTypeGraphsWithoutRecursiveCppCalls) {
  constexpr std::uint64_t kDepth = 16384;
  BinaryWriter body;
  body.Write(std::uint64_t{0});
  body.Write(std::uint64_t{0});
  body.Write(kDepth);
  for (std::uint64_t index = 0; index < kDepth; ++index) {
    internal::ImageElement element;
    element.kind = static_cast<std::uint8_t>(runtime::ValueKind::Object);
    element.object_kind = static_cast<std::uint8_t>(runtime::ObjectKind::Array);
    element.array = index + 1;
    if (index + 1 == kDepth) {
      element.kind = static_cast<std::uint8_t>(runtime::ValueKind::Integer);
      element.object_kind = static_cast<std::uint8_t>(runtime::ObjectKind::Instance);
      element.array = kNoPool;
    }
    WriteElement(body, element);
  }
  body.Write(std::uint64_t{0});
  body.Write(std::uint64_t{0});
  runtime::Isolate isolate;
  const auto loaded = LoadBytecode(ImageWithLayouts(body), isolate);
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(isolate.layout_counts().arrays, kDepth);
}

TEST(BytecodeImageBoundaryTest, RejectsCyclicTypeGraphsAndRollsBackMetadata) {
  BinaryWriter body;
  body.Write(std::uint64_t{0});
  body.Write(std::uint64_t{0});
  body.Write(std::uint64_t{1});
  WriteElement(body, {
                         .kind = static_cast<std::uint8_t>(runtime::ValueKind::Object),
                         .object_kind = static_cast<std::uint8_t>(runtime::ObjectKind::Array),
                         .array = 0,
                     });
  body.Write(std::uint64_t{0});
  body.Write(std::uint64_t{0});
  runtime::Isolate isolate;
  const auto initial = isolate.layout_counts();
  const auto loaded = LoadBytecode(ImageWithLayouts(body), isolate);
  ASSERT_FALSE(loaded);
  EXPECT_NE(loaded.error().message.find("cyclic runtime type layouts"), std::string::npos);
  EXPECT_EQ(isolate.layout_counts(), initial);
}

TEST(BytecodeImageBoundaryTest, RejectsGenericFunctionResultWithoutItsPayloadIdentity) {
  BinaryWriter body;
  constexpr std::size_t kPoolsBeforeSignatures = 4;
  for (std::size_t index = 0; index < kPoolsBeforeSignatures; ++index) {
    body.Write(std::uint64_t{0});
  }
  body.Write(std::uint64_t{1});
  body.Write(std::uint64_t{0});
  body.Write(std::uint8_t{1});
  WriteElement(body, {
                         .kind = static_cast<std::uint8_t>(runtime::ValueKind::Object),
                         .object_kind = static_cast<std::uint8_t>(runtime::ObjectKind::Future),
                     });
  runtime::Isolate isolate;
  const auto loaded = LoadBytecode(ImageWithLayouts(body), isolate);
  ASSERT_FALSE(loaded);
  EXPECT_NE(loaded.error().message.find("payload signature"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(
    InvalidMetadataAndInstructions,
    BytecodeImageBoundaryTest,
    testing::Values(
        ImageFunction{EncodeABC(static_cast<OpCode>(0xff), 0, 0, 0), 1, kNoPool},
        ImageFunction{EncodeABC(OpCode::Return, 1, 0, 0), 1, kNoPool},
        ImageFunction{EncodeABC(OpCode::Return, 0, 0, 0), 0, kNoPool},
        ImageFunction{EncodeABC(OpCode::Return, 0, 0, 0), 1, 0},
        ImageFunction{EncodeABC(OpCode::BuiltinCall,
                                0,
                                static_cast<std::uint8_t>(Builtin::ByteDataSetFloat64),
                                0),
                      3, kNoPool},
        ImageFunction{
            EncodeABC(OpCode::BuiltinCall, 0, static_cast<std::uint8_t>(Builtin::ByteDataView), 0),
            2, kNoPool},
        ImageFunction{EncodeABC(OpCode::LoadNull, 0, 0, 0), 1, kNoPool},
        ImageFunction{EncodeAImm16(OpCode::Jump, 0, 10), 1, kNoPool},
        ImageFunction{EncodeABC(OpCode::Await, 0, 0, 0), 1, kNoPool},
        ImageFunction{
            EncodeABC(OpCode::AsyncCall, 0, static_cast<std::uint8_t>(AsyncOperation::Count), 0), 1,
            kNoPool},
        ImageFunction{
            EncodeABC(OpCode::AsyncCall, 0, static_cast<std::uint8_t>(AsyncOperation::Wait), 0), 1,
            kNoPool}));

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

void WriteEmptyClass(BinaryWriter& body, internal::StandardClass standard, std::uint32_t fields) {
  body.Write(static_cast<std::uint8_t>(standard));
  body.Write(fields);
  body.Write(std::uint64_t{0});
  body.Write(std::uint8_t{0});
  body.Write(std::uint64_t{0});
  body.Write(std::uint64_t{0});
  body.Write(std::uint8_t{0});
  body.Write(kNoPool);
}

TEST(BytecodeImageBoundaryTest, RejectsMalformedStandardDatePartsLayouts) {
  for (const auto fields : {0U, 6U, 8U}) {
    BinaryWriter body;
    body.Write(std::uint64_t{0});
    body.Write(std::uint64_t{1});
    WriteEmptyClass(body, internal::StandardClass::DateParts, fields);
    body.Write(kNoPool);
    runtime::Isolate isolate;
    const auto loaded = LoadBytecode(Image(body.bytes()), isolate);
    ASSERT_FALSE(loaded);
    EXPECT_NE(loaded.error().message.find("invalid standard DateParts layout"), std::string::npos);
  }
}

TEST(BytecodeImageBoundaryTest, FailedLoadingReleasesPartiallyCreatedLayoutMetadata) {
  runtime::Isolate isolate;
  const auto initial = isolate.layout_counts();
  BinaryWriter body;
  body.Write(std::uint64_t{0});
  body.Write(std::uint64_t{1});
  WriteEmptyClass(body, internal::StandardClass::None, 0);
  body.Write(kNoPool);
  // 完整类记录之后数组表长度非法；布局已分配，失败时必须回收。
  EXPECT_FALSE(LoadBytecode(Image(body.bytes()), isolate));
  EXPECT_EQ(isolate.layout_counts(), initial);
}

}  // namespace
}  // namespace vela::vm
