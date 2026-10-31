#include <array>
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

#include "base/execution_status.h"
#include "runtime/exceptions/exception.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/rooted_value.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(ExceptionHeapTest, RootsPayloadAndOwnsMessageInOneAllocation) {
  Isolate isolate;
  auto& heap = isolate.heap();
  constexpr std::array<std::uint32_t, 1> references = {0};
  auto* layout = isolate.CreateClassLayout(1, references);
  const auto text = Value::String(heap.AllocateString("payload"));
  auto* instance = heap.AllocateInstance(*layout, std::span(&text, 1));
  std::string message = "original message";
  auto* exception = heap.AllocateException(*instance, ExecutionStatus::Thrown, message);
  ASSERT_NE(exception, nullptr);
  message.clear();
  {
    const RootedValue root(isolate, Value::Object(exception));
    heap.Collect();
    EXPECT_EQ(heap.object_count(), 3U);
    const auto& data = ExceptionData::From(*exception);
    EXPECT_EQ(data.message(), "original message");
    const auto* field = Heap::GetField(*data.payload, 0);
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->AsString()->value(), "payload");
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

}  // namespace
}  // namespace vela::runtime
