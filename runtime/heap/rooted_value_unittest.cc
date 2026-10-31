#include <optional>
#include <utility>

#include "gtest/gtest.h"

#include "runtime/heap/heap.h"
#include "runtime/heap/rooted_value.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(RootedValueTest, CopiesShareStableRootAndReleaseAfterLastHandle) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  std::optional<RootedValue> first(std::in_place, isolate,
                                   Value::String(heap.AllocateString("host")));
  std::optional<RootedValue> second = first;
  auto moved = std::move(second);
  second.reset();
  first.reset();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
  EXPECT_EQ(moved->value().AsString()->value(), "host");
  moved.reset();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(RootedValueTest, CreatingHandleDuringCollectionPreservesObject) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* string = heap.AllocateString("late root");
  heap.StartCollection();
  {
    const RootedValue handle(isolate, Value::String(string));
    heap.Collect();
    EXPECT_EQ(heap.object_count(), 1U);
    EXPECT_EQ(handle.value().AsString()->value(), "late root");
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

}  // namespace
}  // namespace vela::runtime
