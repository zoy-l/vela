#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/register_storage.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/frame.h"
#include "vm/register_file.h"

namespace vela::vm {
namespace {

constexpr std::int64_t kExpectedInteger = 42;

TEST(RegisterFileTest, StoresInt64AndAllDoubleBitsWithoutNanBoxingOrValuePadding) {
  RegisterFile file;
  constexpr std::size_t kCount = 4;
  auto values = file.Acquire(kCount);
  ASSERT_EQ(values.size(), kCount);
  values.front() = runtime::Value::Integer(std::numeric_limits<std::int64_t>::min());
  constexpr std::uint64_t kNaNBits = 0x7ff8000000000042;
  const auto nan = std::bit_cast<double>(kNaNBits);
  values.subspan(1).front() = runtime::Value::Double(nan);
  values.subspan(2).front() = runtime::Value::Double(-0.0);
  values.subspan(3).front() = runtime::Value::Boolean(true);
  EXPECT_EQ(values.front().AsInteger(), std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(std::bit_cast<std::uint64_t>(values.subspan(1).front().AsDouble()), kNaNBits);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(values.subspan(2).front().AsDouble()),
            std::bit_cast<std::uint64_t>(-0.0));
  EXPECT_TRUE(values.subspan(3).front().AsBoolean());
  EXPECT_LT(runtime::kRegisterSlotBytes, sizeof(runtime::Value));
}

TEST(RegisterFileTest, MovesCopyTheValueAndReuseClearsTagsBeforeGcRootRegistration) {
  runtime::Isolate isolate;
  RegisterFile file;
  auto values = file.Acquire(2);
  values.front() = runtime::Value::String(isolate.heap().AllocateString("retained"));
  values.subspan(1).front() = values.front();
  values.front() = runtime::Value::Null();
  Frame frame;
  frame.Attach(isolate.heap(), values);
  isolate.heap().Collect();
  EXPECT_EQ(values.subspan(1).front().AsString()->value(), "retained");
  frame.Detach();
  values = file.Acquire(1);
  frame.Attach(isolate.heap(), values);
  isolate.heap().Collect();
  EXPECT_TRUE(values.front().IsNull());
  EXPECT_EQ(isolate.heap().object_count(), 0U);
  EXPECT_EQ(file.capacity(), 2U);
}

TEST(RegisterFileTest, ContiguousArgumentCopyPreservesPayloadsAndRootTags) {
  RegisterFile source;
  RegisterFile target;
  auto first = source.Acquire(2);
  auto second = target.Acquire(2);
  first.front() = runtime::Value::Integer(kExpectedInteger);
  first.subspan(1).front() = runtime::Value::Boolean(true);
  second.CopyFrom(first);
  first.front() = runtime::Value::Null();
  EXPECT_EQ(second.front().AsInteger(), 42);
  EXPECT_TRUE(second.subspan(1).front().AsBoolean());
}

}  // namespace
}  // namespace vela::vm
