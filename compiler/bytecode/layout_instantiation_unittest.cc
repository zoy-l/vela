#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "base/layout_description.h"
#include "compiler/ir/preparation_test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/isolate/layout_transaction.h"
#include "runtime/isolate/program_layouts.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::compiler {
namespace {

void ExpectIsolatedNodeLayouts(const runtime::ProgramLayouts& first,
                               const runtime::ProgramLayouts& second,
                               std::size_t node_index) {
  auto* first_node = first.classes.at(node_index);
  auto* second_node = second.classes.at(node_index);
  EXPECT_NE(first_node, second_node);
  ASSERT_EQ(first_node->interfaces().size(), 1U);
  ASSERT_EQ(second_node->interfaces().size(), 1U);
  EXPECT_NE(first_node->interfaces().front(), second_node->interfaces().front());
  const auto nested = std::ranges::find_if(
      first.arrays, [](const auto* array) { return array->element.array != nullptr; });
  ASSERT_NE(nested, first.arrays.end());
  EXPECT_EQ((*nested)->element.array->element.nominal, &first_node->nominal_type());
}

void ExpectNodeArrayCallback(const runtime::TypeSignature& callback,
                             const runtime::ClassLayout* node) {
  const auto& result = callback.result;
  if (!result.has_value()) {
    ADD_FAILURE() << "Callback must retain its result type";
    return;
  }
  ASSERT_NE(result->array, nullptr);
  EXPECT_EQ(result->array->element.nominal, &node->nominal_type());
  ASSERT_EQ(callback.parameters.size(), 1U);
  EXPECT_EQ(callback.parameters.front().type.kind, runtime::ValueKind::String);
}

void ExpectNestedFuture(const runtime::LayoutCache& cache, const runtime::ClassLayout* node) {
  const auto* future = cache.signatures.at("Future<(string) => Node[]>");
  const auto& payload = future->result;
  if (!payload.has_value()) {
    ADD_FAILURE() << "Future must retain its result type";
    return;
  }
  const auto* callback = payload->signature;
  ASSERT_NE(callback, nullptr);
  ExpectNodeArrayCallback(*callback, node);
}

TEST(LayoutInstantiationTest, OwnsNestedTypesAfterFrontendDestructionAndBindsPerIsolate) {
  constexpr std::string_view kSource = R"(
interface Readable { read(): int; }
class Node implements Readable {
  next: Node?;
  read(): int { return 42; }
}
func identity(value: Future<(string) => Node[]>): Future<(string) => Node[]> {
  return value;
}
func main(): void {
  const callback: (Future<(string) => Node[]>) => Future<(string) => Node[]> = identity;
  const nodes: Node[][] = [];
  const view: Readable = Node(next: null);
}
)";
  layout::Program description;
  {
    auto prepared = ir::test::Prepare(kSource);
    ASSERT_TRUE(prepared.Succeeded());
    description = std::move(prepared.program.layouts);
  }
  const auto node = std::ranges::find(description.classes, "Node", &layout::Class::name);
  ASSERT_NE(node, description.classes.end());
  const auto node_index = static_cast<std::size_t>(node - description.classes.begin());
  ASSERT_EQ(node->reference_fields.size(), 1U);
  EXPECT_EQ(node->reference_fields.front(), 0U);

  runtime::Isolate first;
  runtime::Isolate second;
  runtime::LayoutCache first_cache;
  runtime::LayoutCache second_cache;
  runtime::LayoutTransaction first_transaction(first);
  runtime::LayoutTransaction second_transaction(second);
  const auto first_layouts = runtime::InstantiateLayouts(description, first, first_cache);
  const auto second_layouts = runtime::InstantiateLayouts(description, second, second_cache);
  auto* first_node = first_layouts.classes.at(node_index);
  ExpectIsolatedNodeLayouts(first_layouts, second_layouts, node_index);
  ExpectNestedFuture(first_cache, first_node);

  const auto before = first.layout_counts();
  const auto reused = runtime::InstantiateLayouts(description, first, first_cache);
  EXPECT_EQ(reused.classes.at(node_index), first_node);
  EXPECT_EQ(first.layout_counts(), before);
  first_transaction.Commit();
  second_transaction.Commit();
}

TEST(LayoutInstantiationTest, RollsBackUnpublishedLayoutsWithoutChangingSessionCache) {
  auto prepared =
      ir::test::Prepare("class Box { value: string; } func main(values: Box[]): void {}");
  ASSERT_TRUE(prepared.Succeeded());
  runtime::Isolate isolate;
  runtime::LayoutCache committed;
  const auto before = isolate.layout_counts();
  {
    auto pending = committed;
    runtime::LayoutTransaction transaction(isolate);
    const auto layouts = runtime::InstantiateLayouts(prepared.program.layouts, isolate, pending);
    EXPECT_FALSE(layouts.classes.empty());
    EXPECT_GT(isolate.layout_counts().classes, before.classes);
  }
  EXPECT_EQ(isolate.layout_counts(), before);
  EXPECT_TRUE(committed.classes.empty());
  EXPECT_TRUE(committed.arrays.empty());
}

}  // namespace
}  // namespace vela::compiler
