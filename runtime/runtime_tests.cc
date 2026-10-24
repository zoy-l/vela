#include <array>
#include <cstdint>
#include <iostream>

#include "runtime/isolate.h"

namespace {

using vela::runtime::Heap;
using vela::runtime::HeapObject;
using vela::runtime::Isolate;
using vela::runtime::RootRange;
using vela::runtime::Value;

bool TestCollectsUnreachableInstances() {
  Isolate isolate;
  constexpr std::array<std::uint32_t, 1> references = {1};
  auto* layout = isolate.CreateClassLayout(2, references);
  Heap& heap = isolate.heap();

  HeapObject* child = heap.AllocateInstance(*layout);
  Value root_value = Value::Object(heap.AllocateInstance(*layout));
  if (!heap.SetField(*root_value.AsObject(), 1, Value::Object(child))) {
    return false;
  }
  RootRange roots(heap, &root_value, 1);

  if (heap.object_count() != 2) {
    return false;
  }

  heap.Collect();
  if (heap.object_count() != 2) {
    return false;
  }

  root_value = Value::Null();
  heap.Collect();
  return heap.object_count() == 0;
}

bool TestCollectsCyclesWithoutRoots() {
  Isolate isolate;
  constexpr std::array<std::uint32_t, 1> references = {0};
  auto* layout = isolate.CreateClassLayout(1, references);
  Heap& heap = isolate.heap();

  Value first = Value::Object(heap.AllocateInstance(*layout));
  Value second = Value::Object(heap.AllocateInstance(*layout));
  RootRange roots(heap, &first, 1);
  if (!heap.SetField(*first.AsObject(), 0, second) ||
      !heap.SetField(*second.AsObject(), 0, first)) {
    return false;
  }

  heap.Collect();
  if (heap.object_count() != 2) {
    return false;
  }

  first = Value::Null();
  second = Value::Null();
  heap.Collect();
  return heap.object_count() == 0;
}

bool TestWriteBarrierKeepsNewReferenceAlive() {
  Isolate isolate;
  constexpr std::array<std::uint32_t, 1> references = {0};
  auto* layout = isolate.CreateClassLayout(1, references);
  Heap& heap = isolate.heap();

  Value parent = Value::Object(heap.AllocateInstance(*layout));
  RootRange roots(heap, &parent, 1);
  HeapObject* child = heap.AllocateInstance(*layout);

  heap.StartCollection();
  heap.Step(1);
  if (heap.phase() != vela::runtime::GcPhase::Mark) {
    return false;
  }

  if (!heap.SetField(*parent.AsObject(), 0, Value::Object(child))) {
    return false;
  }
  heap.FinishCollection();
  if (heap.object_count() != 2) {
    return false;
  }

  parent = Value::Null();
  heap.Collect();
  return heap.object_count() == 0;
}

bool TestFreeListReusesStorage() {
  Isolate isolate;
  constexpr std::array<std::uint32_t, 0> references = {};
  auto* layout = isolate.CreateClassLayout(1, references);
  Heap& heap = isolate.heap();

  HeapObject* first = heap.AllocateInstance(*layout);
  const std::size_t committed_before_collection = heap.committed_bytes();
  Value root = Value::Object(first);
  RootRange roots(heap, &root, 1);
  root = Value::Null();
  heap.Collect();
  if (heap.object_count() != 0) {
    return false;
  }

  HeapObject* second = heap.AllocateInstance(*layout);
  return second != nullptr && heap.committed_bytes() == committed_before_collection;
}

}  // namespace

int main() {
  const bool passed = TestCollectsUnreachableInstances() && TestCollectsCyclesWithoutRoots() &&
                      TestWriteBarrierKeepsNewReferenceAlive() && TestFreeListReusesStorage();
  if (!passed) {
    std::cerr << "runtime tests failed\n";
    return 1;
  }
  return 0;
}
