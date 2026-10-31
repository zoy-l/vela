#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(InstanceStorageTest, ExternalBlocksAreAccountedAndWeakRegistrationDoesNotKeepObjectsAlive) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* layout = isolate.CreateClassLayout(2, {},
                                           {
                                               .callable = {},
                                               .interfaces = {},
                                               .dispatch_targets = {},
                                               .storage = ClassStorage::Reloadable,
                                               .field_declarations = {{1}, {2}},
                                           });
  const auto bytes = heap.allocated_bytes();
  const std::array fields{Value::Integer(40), Value::Integer(2)};
  auto* object = heap.AllocateInstance(*layout, fields);
  ASSERT_NE(object, nullptr);
  EXPECT_NE(ObjectFields(object), StorageAfter<Value>(*object));
  EXPECT_GT(heap.allocated_bytes() - bytes, sizeof(HeapObject) + (fields.size() * sizeof(Value)));
  EXPECT_GT(heap.instance_field_bytes(), fields.size() * sizeof(Value));
  EXPECT_EQ(heap.instance_type_count(), 1U);
  EXPECT_EQ(Heap::GetField(*object, 0)->AsInteger(), 40);
  std::vector<Value> instances;
  heap.AppendInstances(layout->nominal_type(), instances);
  ASSERT_EQ(instances.size(), 1U);
  EXPECT_EQ(instances.front().AsObject(), object);
  heap.Collect();
  instances.clear();
  heap.AppendInstances(layout->nominal_type(), instances);
  EXPECT_TRUE(instances.empty());
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.allocated_bytes(), bytes);
  EXPECT_EQ(heap.instance_field_bytes(), 0U);
  EXPECT_EQ(heap.instance_type_count(), 0U);
}

TEST(InstanceStorageTest,
     PreparedMigrationKeepsTheOriginalBlockUntilCommitAndClearsDeletedReferences) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  auto* nominal =
      isolate.CreateNominalType({.kind = NominalKind::Class, .interfaces = {}, .declaration = {}});
  auto* leaf = isolate.CreateClassLayout(0, {});
  auto* original = isolate.CreateClassLayout(2, std::array<std::uint32_t, 1>{1},
                                             {
                                                 .callable = {},
                                                 .interfaces = {},
                                                 .dispatch_targets = {},
                                                 .nominal = nominal,
                                                 .storage = ClassStorage::Reloadable,
                                                 .field_declarations = {{1}, {2}},
                                             });
  Value root = Value::Object(heap.AllocateInstance(*original));
  const RootRange roots(heap, &root, 1);
  auto* object = root.AsObject();
  ASSERT_TRUE(heap.SetField(*object, 0, Value::Integer(42)));
  ASSERT_TRUE(heap.SetField(*object, 1, Value::Object(heap.AllocateInstance(*leaf))));
  auto* replacement = isolate.CreateClassLayout(3, std::array<std::uint32_t, 1>{2},
                                                {
                                                    .callable = {},
                                                    .interfaces = {},
                                                    .dispatch_targets = {},
                                                    .nominal = nominal,
                                                    .storage = ClassStorage::Reloadable,
                                                    .field_declarations = {{1}, {}, {3}},
                                                });
  const auto* old_fields = ObjectFields(object);
  {
    const auto candidate = heap.PrepareInstanceMigration(*object, *replacement);
    ASSERT_NE(candidate, nullptr);
    heap.Collect();
    EXPECT_EQ(Heap::ClassOf(*object), original);
    EXPECT_EQ(ObjectFields(object), old_fields);
    EXPECT_EQ(heap.object_count(), 2U);
  }
  auto candidate = heap.PrepareInstanceMigration(*object, *replacement);
  ASSERT_NE(candidate, nullptr);
  candidate->Commit();
  EXPECT_EQ(root.AsObject(), object);
  EXPECT_EQ(Heap::ClassOf(*object), replacement);
  EXPECT_NE(ObjectFields(object), old_fields);
  EXPECT_EQ(Heap::GetField(*object, 0)->AsInteger(), 42);
  EXPECT_TRUE(Heap::GetField(*object, 1)->IsNull());
  EXPECT_TRUE(Heap::GetField(*object, 2)->IsNull());
  ASSERT_TRUE(heap.SetField(*object, 2, Value::Object(heap.AllocateInstance(*leaf))));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  root = Value::Null();
  candidate.reset();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(InstanceStorageTest, ReplacingABlackObjectsBlockPreservesNewReferencesDuringIncrementalGc) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* nominal =
      isolate.CreateNominalType({.kind = NominalKind::Class, .interfaces = {}, .declaration = {}});
  auto* leaf = isolate.CreateClassLayout(0, {});
  auto* original = isolate.CreateClassLayout(1, {},
                                             {
                                                 .callable = {},
                                                 .interfaces = {},
                                                 .dispatch_targets = {},
                                                 .nominal = nominal,
                                                 .storage = ClassStorage::Reloadable,
                                                 .field_declarations = {{1}},
                                             });
  Value root = Value::Object(heap.AllocateInstance(*original));
  const RootRange roots(heap, &root, 1);
  auto* object = root.AsObject();
  auto* replacement = isolate.CreateClassLayout(2, std::array<std::uint32_t, 1>{1},
                                                {
                                                    .callable = {},
                                                    .interfaces = {},
                                                    .dispatch_targets = {},
                                                    .nominal = nominal,
                                                    .storage = ClassStorage::Reloadable,
                                                    .field_declarations = {{1}, {2}},
                                                });
  heap.StartCollection();
  heap.Step(1);
  ASSERT_EQ(heap.phase(), GcPhase::Mark);
  const auto migration = heap.PrepareInstanceMigration(*object, *replacement);
  ASSERT_NE(migration, nullptr);
  migration->Commit();
  ASSERT_TRUE(heap.SetField(*object, 1, Value::Object(heap.AllocateInstance(*leaf))));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
}

TEST(InstanceStorageTest, InstanceListsRemainValidAcrossTypeIndexRehashAndMiddleRemoval) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* layout = isolate.CreateClassLayout(0, {},
                                           {
                                               .callable = {},
                                               .interfaces = {},
                                               .dispatch_targets = {},
                                               .storage = ClassStorage::Reloadable,
                                               .field_declarations = {},
                                           });
  std::array roots{Value::Object(heap.AllocateInstance(*layout)), Value::Null(), Value::Null()};
  const RootRange range(heap, roots.data(), roots.size());
  roots.at(1) = Value::Object(heap.AllocateInstance(*layout));
  roots.back() = Value::Object(heap.AllocateInstance(*layout));
  constexpr std::size_t kTypes = 128;
  for (std::size_t index = 0; index < kTypes; ++index) {
    auto* other = isolate.CreateClassLayout(0, {},
                                            {
                                                .callable = {},
                                                .interfaces = {},
                                                .dispatch_targets = {},
                                                .storage = ClassStorage::Reloadable,
                                                .field_declarations = {},
                                            });
    ASSERT_NE(heap.AllocateInstance(*other), nullptr);
  }
  roots.at(1) = Value::Null();
  heap.Collect();
  std::vector<Value> instances;
  heap.AppendInstances(layout->nominal_type(), instances);
  EXPECT_EQ(instances.size(), 2U);
  EXPECT_EQ(heap.object_count(), 2U);
  roots.fill(Value::Null());
  heap.Collect();
  instances.clear();
  heap.AppendInstances(layout->nominal_type(), instances);
  EXPECT_TRUE(instances.empty());
}

}  // namespace
}  // namespace vela::runtime
