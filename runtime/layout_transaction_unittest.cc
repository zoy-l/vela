#include "gtest/gtest.h"

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/layout_transaction.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

TEST(LayoutTransactionTest, RollbackReleasesOnlyNewMetadataAndPreservesExistingObjects) {
  Isolate isolate(1);
  auto* original = isolate.CreateClassLayout(1, {});
  auto* object = isolate.heap().AllocateInstance(*original);
  ASSERT_NE(object, nullptr);
  Value value = Value::Object(object);
  RootRange root(isolate.heap(), &value, 1);
  const auto initial = isolate.layout_counts();
  {
    LayoutTransaction transaction(isolate);
    auto* added = isolate.CreateClassLayout(1, {});
    const auto* array =
        isolate.CreateArrayLayout({.element_kind = ValueKind::Object, .element_class = added});
    const auto* collection = isolate.CreateCollectionLayout({
        .keys =
            {
                .element_kind = ValueKind::Object,
                .element_array = array,
                .element_object_kind = ObjectKind::Array,
            },
    });
    ASSERT_NE(collection, nullptr);
    EXPECT_GT(isolate.layout_counts().classes, initial.classes);
  }
  EXPECT_EQ(isolate.layout_counts(), initial);
  isolate.heap().Collect();
  EXPECT_EQ(Heap::ClassOf(*value.AsObject()), original);
}

TEST(LayoutTransactionTest, CommittedMetadataCanOwnInstancesAfterTheTransactionEnds) {
  Isolate isolate;
  const auto initial = isolate.layout_counts();
  ClassLayout* layout = nullptr;
  {
    LayoutTransaction transaction(isolate);
    layout = isolate.CreateClassLayout(1, {});
    transaction.Commit();
  }
  EXPECT_EQ(isolate.layout_counts().classes, initial.classes + 1);
  auto* object = isolate.heap().AllocateInstance(*layout);
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(Heap::ClassOf(*object), layout);
}

}  // namespace
}  // namespace vela::runtime
