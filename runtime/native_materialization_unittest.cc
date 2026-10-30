#include "runtime/native_materialization.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_control.h"
#include "runtime/native_deoptimization.h"
#include "runtime/native_image.h"
#include "runtime/native_program_state.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

template <class T, std::size_t Size>
NativeTable<T> Table(const std::array<T, Size>& values) {
  return {.data = values.data(), .size = values.size()};
}

TEST(NativeMaterializationTest, EveryAllocationFailureLeavesAliasesUntouchedAndReclaimsTheGraph) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  auto* layout = isolate.CreateClassLayout(4, std::array<std::uint32_t, 3>{1, 2, 3},
                                           {
                                               .callable = {},
                                               .interfaces = {},
                                               .dispatch_targets = {},
                                               .storage = ClassStorage::Reloadable,
                                               .field_declarations = {{1}, {2}, {3}, {4}},
                                           });
  const NativeImage image;
  auto state = std::make_unique<NativeProgramState>();
  state->isolate = &isolate;
  state->image = &image;
  state->layouts.classes.push_back(layout);
  Value program = Value::Object(heap.AllocateNativeProgram(std::move(state)));
  ASSERT_NE(program.AsObject(), nullptr);
  const RootRange program_root(heap, &program, 1);
  Value text = Value::String(heap.AllocateString("retained"));
  const RootRange text_root(heap, &text, 1);
  constexpr std::int64_t kFirstValue = 20;
  constexpr std::int64_t kSecondValue = 22;
  std::array values{
      EncodeNativeValue(Value::Integer(kFirstValue)),
      EncodeNativeValue(Value::Integer(kSecondValue)),
      EncodeNativeValue(text),
      EncodeNativeValue(Value::Integer(0)),
      EncodeNativeValue(Value::Integer(1)),
      EncodeNativeValue(Value::Integer(2)),
  };
  std::array<HeapObject*, 2> references{};
  std::array<HeapObject**, 2> addresses{&references.front(), &references.back()};
  std::array<Value, 2> shadow;
  const RootRange shadow_roots(heap, shadow.data(), shadow.size());
  const std::array first_fields{
      NativeVirtualField{.declaration = {1}, .value = 0},
      NativeVirtualField{.declaration = {2}, .value = 2},
      NativeVirtualField{.declaration = {3}, .object = 1},
      NativeVirtualField{.declaration = {4}, .object = 1},
  };
  const std::array second_fields{
      NativeVirtualField{.declaration = {1}, .value = 1},
      NativeVirtualField{.declaration = {2}, .value = 2},
      NativeVirtualField{.declaration = {3}, .object = 0},
      NativeVirtualField{.declaration = {4}, .object = 0},
  };
  const std::array objects{
      NativeVirtualObject{.class_index = 0, .storage = 0, .root = 0, .fields = Table(first_fields)},
      NativeVirtualObject{
          .class_index = 0,
          .storage = 1,
          .root = 1,
          .fields = Table(second_fields),
      },
  };
  const std::array aliases{
      NativeVirtualAlias{.reg = 3, .object = 0},
      NativeVirtualAlias{.reg = 4, .object = 0},
      NativeVirtualAlias{.reg = 5, .object = 1},
  };
  const NativeVirtualGraph graph{.objects = Table(objects), .aliases = Table(aliases)};
  NativeCheckpoint checkpoint;
  checkpoint.values = values.data();
  checkpoint.value_count = values.size();
  checkpoint.roots = shadow.data();
  checkpoint.root_count = shadow.size();
  checkpoint.virtual_graph = &graph;
  checkpoint.virtual_references = addresses.data();
  checkpoint.reference_count = addresses.size();
  checkpoint.program = program.AsObject();
  heap.Collect();
  const auto baseline_bytes = heap.allocated_bytes();
  const auto baseline_objects = heap.object_count();
  constexpr std::size_t kMaximumPreparationBytes = 4096;
  bool reached_success = false;
  for (std::size_t allowance = 0; allowance <= kMaximumPreparationBytes; ++allowance) {
    heap.SetAllocationLimit(baseline_bytes + allowance);
    const NativeInitializationScope initialization(isolate.native_control());
    auto prepared = NativeMaterialization::Prepare(heap, checkpoint);
    heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(references.front(), nullptr);
    EXPECT_EQ(references.back(), nullptr);
    EXPECT_EQ(DecodeNativeValue(values.at(3)).AsInteger(), 0);
    EXPECT_EQ(DecodeNativeValue(values.at(4)).AsInteger(), 1);
    EXPECT_EQ(DecodeNativeValue(values.at(5)).AsInteger(), 2);
    if (prepared) {
      EXPECT_EQ((*prepared)->object_count(), 2U);
      prepared->reset();
      reached_success = true;
    } else {
      EXPECT_EQ(prepared.error(), ExecutionStatus::OutOfMemory);
    }
    heap.Collect();
    EXPECT_EQ(heap.allocated_bytes(), baseline_bytes) << allowance;
    EXPECT_EQ(heap.object_count(), baseline_objects) << allowance;
    EXPECT_EQ(heap.instance_field_bytes(), 0U) << allowance;
    EXPECT_EQ(heap.instance_type_count(), 0U) << allowance;
    if (reached_success) {
      break;
    }
  }
  ASSERT_TRUE(reached_success);
  auto prepared = NativeMaterialization::Prepare(heap, checkpoint);
  ASSERT_TRUE(prepared);
  (*prepared)->Commit();
  EXPECT_FALSE(NativeMaterialization::HasVirtualObjects(checkpoint));
  auto* first = references.front();
  auto* second = references.back();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(DecodeNativeValue(values.at(3)).AsObject(), first);
  EXPECT_EQ(DecodeNativeValue(values.at(4)).AsObject(), first);
  EXPECT_EQ(DecodeNativeValue(values.at(5)).AsObject(), second);
  EXPECT_EQ(Heap::GetField(*first, 0)->AsInteger(), 20);
  EXPECT_EQ(Heap::GetField(*second, 0)->AsInteger(), 22);
  EXPECT_EQ(Heap::GetField(*first, 2)->AsObject(), second);
  EXPECT_EQ(Heap::GetField(*first, 3)->AsObject(), second);
  EXPECT_EQ(Heap::GetField(*second, 2)->AsObject(), first);
  EXPECT_EQ(Heap::GetField(*second, 3)->AsObject(), first);
  prepared->reset();
  text = Value::Null();
  heap.Collect();
  EXPECT_EQ(Heap::GetField(*first, 1)->AsString()->value(), "retained");
  EXPECT_EQ(heap.object_count(), baseline_objects + 2);
  shadow.fill(Value::Null());
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
  EXPECT_EQ(heap.instance_field_bytes(), 0U);
  EXPECT_EQ(heap.instance_type_count(), 0U);
}

}  // namespace
}  // namespace vela::runtime
