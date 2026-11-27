#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/inline_layout.h"
#include "runtime/heap/field_storage.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(InstanceStorageTest, NestedStructSnapshotsReportOomAndWritesTraceReferenceLeaves) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  auto* nested = isolate.CreateClassLayout(
      2, std::array<std::uint32_t, 1>{1},
      {
          .callable = {},
          .interfaces = {},
          .nominal = isolate.CreateNominalType(
              {.kind = NominalKind::Struct, .interfaces = {}, .declaration = {}}),
          .field_declarations = {},
          .value_slots = {InlineSlotKind::Integer, InlineSlotKind::String},
      });
  auto* outer = isolate.CreateClassLayout(
      4, std::array<std::uint32_t, 1>{2},
      {
          .callable = {},
          .interfaces = {},
          .nominal = isolate.CreateNominalType(
              {.kind = NominalKind::Struct, .interfaces = {}, .declaration = {}}),
          .field_declarations = {},
          .value_slots =
              {
                  InlineSlotKind::Boolean,
                  InlineSlotKind::Integer,
                  InlineSlotKind::String,
                  InlineSlotKind::Integer,
              },
      });
  outer->BindValueFields({{.offset = 0, .layout = nested, .nullable = true}, {.offset = 3}});
  std::array values{Value::Object(heap.AllocateInstance(*outer)), Value::Null(), Value::Null()};
  ASSERT_NE(values[0].AsObject(), nullptr);
  const RootRange roots(heap, values.data(), values.size());
  values[1] = Value::Object(heap.AllocateInstance(*nested));
  ASSERT_NE(values[1].AsObject(), nullptr);
  ASSERT_TRUE(heap.SetField(*values[1].AsObject(), 0, Value::Integer(40)));
  ASSERT_TRUE(
      heap.SetField(*values[1].AsObject(), 1, Value::String(heap.AllocateString("retained"))));
  auto& object = *values[0].AsObject();
  heap.StartCollection();
  heap.WriteValueField(object, 0, values[1]);
  heap.WriteValueField(object, 1, Value::Integer(2));
  values[1] = Value::Null();
  heap.FinishCollection();
  heap.SetAllocationCountLimit(0);
  const auto failed = heap.ReadValueField(object, 0);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), ExecutionStatus::OutOfMemory);
  EXPECT_EQ(heap.ReadValueField(object, 1)->AsInteger(), 2);
  heap.SetAllocationCountLimit(std::numeric_limits<std::size_t>::max());
  const auto copy = heap.ReadValueField(object, 0);
  ASSERT_TRUE(copy);
  values[2] = *copy;
  ASSERT_TRUE(heap.SetField(*copy->AsObject(), 0, Value::Integer(99)));
  EXPECT_EQ(Heap::LoadField(object, 1).AsInteger(), 40);
  heap.WriteValueField(object, 0, Value::Null());
  heap.Collect();
  EXPECT_TRUE(heap.ReadValueField(object, 0)->IsNull());
  EXPECT_EQ(Heap::LoadField(object, 1).AsInteger(), 0);
  EXPECT_TRUE(Heap::LoadField(object, 2).IsNull());
  EXPECT_EQ(Heap::LoadField(*values[2].AsObject(), 1).AsString()->value(), "retained");
}

TEST(InstanceStorageTest, InlineFieldsReuseTheMetadataTailAndKeepSixScalarFieldsInOneSmallBlock) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  constexpr std::uint32_t kFieldCount = 6;
  const auto* layout = isolate.CreateClassLayout(kFieldCount, {});
  const auto baseline = heap.allocated_bytes();
  const std::array fields{
      Value::Double(1.0), Value::Double(2.0), Value::Double(3.0),
      Value::Double(4.0), Value::Double(5.0), Value::Double(6.0),
  };
  auto* object = heap.AllocateInstance(*layout, fields);
  ASSERT_NE(object, nullptr);
  const auto value = Value::Object(object);
  const RootRange root(heap, &value, 1);
  const auto header = std::as_bytes(std::span(object, 1));
  EXPECT_EQ(ObjectFieldStorage(*object).data(),
            header.subspan(NativeMemoryLayout::kInlineObjectFields).data());
  EXPECT_LT(NativeMemoryLayout::kInlineObjectFields, sizeof(HeapObject));
  EXPECT_LE(heap.allocated_bytes() - baseline, 96U);
  for (std::uint32_t field = 0; field < fields.size(); ++field) {
    EXPECT_EQ(Heap::LoadField(*object, field).AsDouble(), fields[field].AsDouble());
    ASSERT_TRUE(heap.SetField(*object, field, Value::Integer(field)));
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
  EXPECT_EQ(Heap::ClassOf(*object), layout);
  for (std::uint32_t field = 0; field < fields.size(); ++field) {
    EXPECT_EQ(Heap::LoadField(*object, field).AsInteger(), field);
  }
}

TEST(InstanceStorageTest, ReferenceInReusedMetadataSlotIsTracedThroughCollection) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  const std::array reference_fields{std::uint32_t{0}};
  const auto* layout = isolate.CreateClassLayout(1, reference_fields);
  auto value = Value::Object(heap.AllocateInstance(*layout));
  ASSERT_NE(value.AsObject(), nullptr);
  const RootRange root(heap, &value, 1);
  const auto child = Value::String(heap.AllocateString("first field root"));
  ASSERT_TRUE(heap.SetField(*value.AsObject(), 0, child));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(Heap::LoadField(*value.AsObject(), 0).AsString()->value(), "first field root");
  ASSERT_TRUE(heap.SetField(*value.AsObject(), 0, Value::Null()));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
}

TEST(InstanceStorageTest, ExternalBlocksAreAccountedAndWeakRegistrationDoesNotKeepObjectsAlive) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* layout = isolate.CreateClassLayout(2, {},
                                           {
                                               .callable = {},
                                               .interfaces = {},
                                               .storage = ClassStorage::Reloadable,
                                               .field_declarations = {{1}, {2}},
                                               .value_slots = {},
                                           });
  const auto bytes = heap.allocated_bytes();
  const std::array fields{Value::Integer(40), Value::Integer(2)};
  auto* object = heap.AllocateInstance(*layout, fields);
  ASSERT_NE(object, nullptr);
  EXPECT_NE(ObjectFieldStorage(*object).data(), StorageAfter<std::byte>(*object));
  const auto field_bytes = FieldStorage::Size(layout->field_count());
  EXPECT_GT(heap.allocated_bytes() - bytes, sizeof(HeapObject) + field_bytes);
  EXPECT_GT(heap.instance_field_bytes(), field_bytes);
  EXPECT_EQ(heap.instance_type_count(), 1U);
  const auto field = Heap::GetField(*object, 0);
  ASSERT_TRUE(field.has_value());
  EXPECT_EQ(field->AsInteger(), 40);
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
                                                 .nominal = nominal,
                                                 .storage = ClassStorage::Reloadable,
                                                 .field_declarations = {{1}, {2}},
                                                 .value_slots = {},
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
                                                    .nominal = nominal,
                                                    .storage = ClassStorage::Reloadable,
                                                    .field_declarations = {{1}, {}, {3}},
                                                    .value_slots = {},
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
  ASSERT_EQ(Heap::ClassOf(*object), replacement);
  EXPECT_NE(ObjectFieldStorage(*object).data(), old_fields);
  EXPECT_EQ(Heap::LoadField(*object, 0).AsInteger(), 42);
  EXPECT_TRUE(Heap::LoadField(*object, 1).IsNull());
  EXPECT_TRUE(Heap::LoadField(*object, 2).IsNull());
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
                                                 .nominal = nominal,
                                                 .storage = ClassStorage::Reloadable,
                                                 .field_declarations = {{1}},
                                                 .value_slots = {},
                                             });
  Value root = Value::Object(heap.AllocateInstance(*original));
  const RootRange roots(heap, &root, 1);
  auto* object = root.AsObject();
  auto* replacement = isolate.CreateClassLayout(2, std::array<std::uint32_t, 1>{1},
                                                {
                                                    .callable = {},
                                                    .interfaces = {},
                                                    .nominal = nominal,
                                                    .storage = ClassStorage::Reloadable,
                                                    .field_declarations = {{1}, {2}},
                                                    .value_slots = {},
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

TEST(InstanceStorageTest, MigrationToAShorterLayoutDropsRemovedFields) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* nominal =
      isolate.CreateNominalType({.kind = NominalKind::Class, .interfaces = {}, .declaration = {}});
  ClassLayoutOptions options{
      .callable = {},
      .interfaces = {},
      .nominal = nominal,
      .storage = ClassStorage::Reloadable,
      .field_declarations = {{1}, {2}},
      .value_slots = {},
  };
  auto* original = isolate.CreateClassLayout(2, std::array<std::uint32_t, 1>{1}, options);
  Value value = Value::Object(heap.AllocateInstance(*original));
  const RootRange root(heap, &value, 1);
  auto& object = *value.AsObject();
  ASSERT_TRUE(heap.SetField(object, 0, Value::Integer(42)));
  ASSERT_TRUE(heap.SetField(object, 1, Value::String(heap.AllocateString("removed"))));
  options.field_declarations.resize(1);
  auto* replacement = isolate.CreateClassLayout(1, {}, options);
  auto migration = heap.PrepareInstanceMigration(object, *replacement);
  ASSERT_TRUE(migration);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  (*migration)->Commit();
  EXPECT_EQ(Heap::ClassOf(object), replacement);
  EXPECT_EQ(Heap::LoadField(object, 0).AsInteger(), 42);
  EXPECT_FALSE(Heap::GetField(object, 1));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
}

TEST(InstanceStorageTest, InstanceListsRemainValidAcrossTypeCreationAndMiddleRemoval) {
  Isolate isolate;
  auto& heap = isolate.heap();
  ClassLayoutOptions options;
  options.storage = ClassStorage::Reloadable;
  auto* layout = isolate.CreateClassLayout(0, {}, options);
  std::array roots{Value::Object(heap.AllocateInstance(*layout)), Value::Null(), Value::Null()};
  const RootRange range(heap, roots.data(), roots.size());
  roots.at(1) = Value::Object(heap.AllocateInstance(*layout));
  roots.back() = Value::Object(heap.AllocateInstance(*layout));
  constexpr std::size_t kTypes = 128;
  for (std::size_t index = 0; index < kTypes; ++index) {
    auto* other = isolate.CreateClassLayout(0, {}, options);
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
