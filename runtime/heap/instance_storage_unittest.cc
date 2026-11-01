#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "runtime/heap/field_storage.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"
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
  EXPECT_NE(ObjectFieldStorage(*object).data(), StorageAfter<std::byte>(*object));
  EXPECT_GT(heap.allocated_bytes() - bytes,
            sizeof(HeapObject) + FieldStorage::Size(layout->field_count()));
  EXPECT_GT(heap.instance_field_bytes(), FieldStorage::Size(layout->field_count()));
  EXPECT_EQ(heap.instance_type_count(), 1U);
  const auto object_field_0 = Heap::GetField(*object, 0);
  if (!object_field_0.has_value()) {
    FAIL() << "Expected object_field_0 to have a value";
  }
  EXPECT_EQ(object_field_0->AsInteger(), 40);
  std::vector<Value> instances;
  Heap::AppendInstances(layout->nominal_type(), instances);
  ASSERT_EQ(instances.size(), 1U);
  EXPECT_EQ(instances.front().AsObject(), object);
  heap.Collect();
  instances.clear();
  Heap::AppendInstances(layout->nominal_type(), instances);
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
  const auto* old_fields = ObjectFieldStorage(*object).data();
  {
    const auto candidate = heap.PrepareInstanceMigration(*object, *replacement);
    ASSERT_TRUE(candidate);
    heap.Collect();
    EXPECT_EQ(Heap::ClassOf(*object), original);
    EXPECT_EQ(ObjectFieldStorage(*object).data(), old_fields);
    EXPECT_EQ(heap.object_count(), 2U);
  }
  auto candidate = heap.PrepareInstanceMigration(*object, *replacement);
  ASSERT_TRUE(candidate);
  (*candidate)->Commit();
  EXPECT_EQ(root.AsObject(), object);
  EXPECT_EQ(Heap::ClassOf(*object), replacement);
  EXPECT_NE(ObjectFieldStorage(*object).data(), old_fields);
  const auto object_field_0 = Heap::GetField(*object, 0);
  if (!object_field_0.has_value()) {
    FAIL() << "Expected object_field_0 to have a value";
  }
  EXPECT_EQ(object_field_0->AsInteger(), 42);
  const auto object_field_1 = Heap::GetField(*object, 1);
  if (!object_field_1.has_value()) {
    FAIL() << "Expected object_field_1 to have a value";
  }
  EXPECT_TRUE(object_field_1->IsNull());
  const auto object_field_2 = Heap::GetField(*object, 2);
  if (!object_field_2.has_value()) {
    FAIL() << "Expected object_field_2 to have a value";
  }
  EXPECT_TRUE(object_field_2->IsNull());
  ASSERT_TRUE(heap.SetField(*object, 2, Value::Object(heap.AllocateInstance(*leaf))));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  root = Value::Null();
  candidate->reset();
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
  ASSERT_TRUE(migration);
  (*migration)->Commit();
  ASSERT_TRUE(heap.SetField(*object, 1, Value::Object(heap.AllocateInstance(*leaf))));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
}

TEST(InstanceStorageTest, InstanceListsRemainValidAcrossTypeCreationAndMiddleRemoval) {
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
  Heap::AppendInstances(layout->nominal_type(), instances);
  EXPECT_EQ(instances.size(), 2U);
  EXPECT_EQ(heap.object_count(), 2U);
  roots.fill(Value::Null());
  heap.Collect();
  instances.clear();
  Heap::AppendInstances(layout->nominal_type(), instances);
  EXPECT_TRUE(instances.empty());
}

TEST(InstanceStorageTest, PreparedFieldStorageRootsRetainSeparatePayloadsAndKinds) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateClassLayout(0, {});
  constexpr std::uint32_t kFields = 2;
  alignas(InstanceStorage)
      std::array<std::byte, sizeof(InstanceStorage) + FieldStorage::Size(kFields)>
          bytes{};
  void* memory = bytes.data();
  auto* storage = std::construct_at(static_cast<InstanceStorage*>(memory));
  storage->count = kFields;
  const auto fields = storage->fields();
  FieldStorage::Store(fields, kFields, 0, Value::Object(heap.AllocateInstance(*layout)));
  FieldStorage::Store(fields, kFields, 1, Value::String(heap.AllocateString("retained")));
  RootRange roots;
  roots.Attach(heap, *storage);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_TRUE(FieldStorage::Load(fields, kFields, 0).IsObject());
  EXPECT_EQ(FieldStorage::Load(fields, kFields, 1).AsString()->value(), "retained");
  roots.Detach();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  std::destroy_at(storage);
}

}  // namespace
}  // namespace vela::runtime
