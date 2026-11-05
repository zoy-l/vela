#include <optional>
#include <utility>

#include "gtest/gtest.h"

#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(HostRootTest, CopiesShareStableRootAndReleaseAfterLastHandle) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  auto created = HostRoot::Create(isolate, Value::String(heap.AllocateString("host")));
  ASSERT_TRUE(created);
  std::optional<HostRoot> first(std::move(*created));
  std::optional<HostRoot> second = first;
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

TEST(HostRootTest, CreatingHandleDuringCollectionPreservesObject) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* string = heap.AllocateString("late root");
  heap.StartCollection();
  {
    const auto handle_storage = HostRoot::Create(isolate, Value::String(string));
    ASSERT_TRUE(handle_storage);
    const auto& handle = *handle_storage;
    heap.Collect();
    EXPECT_EQ(heap.object_count(), 1U);
    EXPECT_EQ(handle.value().AsString()->value(), "late root");
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

}  // namespace
}  // namespace vela::runtime
