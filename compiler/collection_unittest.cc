#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class CollectionLanguageTest : public testing::TestWithParam<std::string_view> {};

TEST_P(CollectionLanguageTest, ExecutesTypedCollectionsUnderGcPressure) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(GetParam(), isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ": " << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto retained = isolate.heap().object_count();
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << static_cast<int>(result.status);
  EXPECT_EQ(result.value.AsInteger(), 42);
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), retained);
}

INSTANTIATE_TEST_SUITE_P(OrderedStorage,
                         CollectionLanguageTest,
                         testing::Values(
                             R"(
func main(): int {
  const values = Set<int>();
  values.add(40); values.add(1); values.add(2); values.delete(1); values.add(1);
  if (!values.add(40) && values.has(1) && values.size == 3) {
    const ordered = values.values();
    return ordered[0] + ordered[1];
  }
  return 0;
}
)",
                             R"(
func main(): int {
  const values: Map<string, int> = Map();
  values.set(key: "a" + "b", value: 40);
  values.set("next", 2);
  values.set("ab", 41);
  if (values.size != 2 || !values.has("ab")) { return 0; }
  const first = values.get("ab");
  const missing = values.get("missing");
  const ordered = values.keys();
  if (first != null && missing == null && ordered[0] == "ab") { return first + 1; }
  return 0;
}
)",
                             R"(
class Item { value: int; }
func main(): int {
  const key = Item(value: 40);
  const values = Map<Item, string>();
  values.set(key, "a" + "b");
  if (values.has(Item(value: 40))) { return 0; }
  const stored = values.get(key);
  if (stored != null) { return key.value + stored.length; }
  return 0;
}
)",
                             R"(
func main(): int {
  const inner = Set<int>(); inner.add(42);
  const outer = Map<string, Set<int>>(); outer.set("items", inner);
  const nested: Map<string, Set<int>>[] = [outer];
  const copy = nested[0].get("items");
  if (copy != null) { return copy.values()[0]; }
  return 0;
}
)",
                             R"(
func main(): int {
  const values = Map<int?, int?>();
  values.set(null, null); values.set(40, 42);
  if (!values.has(null) || values.get(null) != null) { return 0; }
  const result = values.get(40);
  values.clear();
  if (result != null && values.size == 0) { return result; }
  return 0;
}
)",
                             R"(
func main(): int {
  const infinity: double = 1e308 * 1e308;
  const nan: double = infinity - infinity;
  const values = Set<double>();
  values.add(0.0); values.add(-0.0); values.add(nan); values.add(-nan);
  if (values.has(nan) && values.size == 2) { return 42; }
  return 0;
}
)",
                             R"(
func main(): int {
  const values = Map<int, Expected<int, string>>();
  values.set(1, ok(42));
  const result = values.get(1);
  if (result != null) { return result.value(); }
  return 0;
}
)"));

class CollectionTypeErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(CollectionTypeErrorTest, RejectsInvalidKeysValuesAndImplicitConversions) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StaticTypes,
    CollectionTypeErrorTest,
    testing::Values("func main(): void { const values = Set<int>(); values.add(1.0); }",
                    "func main(): void { const values = Map<int, int>(); values.set(1, 2.0); }",
                    "func main(): void { const values = Set(); }",
                    "func main(): void { const values = Map<int>(); }",
                    "func main(): void { const values = Set<void>(); }",
                    "func main(): void { const values: Set<double> = Set<int>(); }",
                    "func main(): int { const values = Map<int, int>(); return values.get(1); }",
                    "func ok<T>(value:T):T { return value; } func main(): void {}",
                    "type MaybeSet = Set<int>?; func main(): void { const values = MaybeSet(); }"));

}  // namespace
}  // namespace vela::compiler
