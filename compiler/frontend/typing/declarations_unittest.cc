#include <string_view>

#include "gtest/gtest.h"

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/test_support/checker_test_support.h"

namespace vela::frontend {
namespace {

class DeclarationErrorTest : public testing::TestWithParam<std::string_view> {};

TEST(DeclarationTest, StructLayoutsFlattenNestedValuesAndRetainReferenceFields) {
  auto parsed = ParseTestSource(R"(
struct Position { x: double; y: double; }
class Entity { id: int; }
struct Component { position: Position; owner: Entity?; next: Position?; }
)");
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded());
  const auto& component = checked.model.classes.at("Component").type;
  ASSERT_NE(component.value_layout, nullptr);
  const auto& layout = *component.value_layout;
  ASSERT_EQ(layout.fields.size(), 3U);
  ASSERT_EQ(layout.slots.size(), 6U);
  EXPECT_EQ(layout.offsets[0], 0U);
  EXPECT_EQ(layout.offsets[1], 2U);
  EXPECT_EQ(layout.offsets[2], 3U);
  EXPECT_EQ(layout.slots[0].kind, TypeDescriptor::Kind::Double);
  EXPECT_EQ(layout.slots[2].kind, TypeDescriptor::Kind::Class);
  EXPECT_TRUE(layout.slots[2].nullable);
  EXPECT_EQ(layout.slots[2].value_layout, nullptr);
  EXPECT_EQ(layout.slots[3].kind, TypeDescriptor::Kind::Bool);
}

TEST(DeclarationTest, StructRejectsInheritanceRecursiveLayoutsAndWritesThroughConstValues) {
  for (const std::string_view source : {
           "struct S extends B {} class B {}",
           "struct S {} class C extends S {}",
           "struct S { value: S; }",
           "struct S { value: S?; }",
           "struct A { b: B; } struct B { a: A; }",
           "struct S { x: int; } func main(): int { const s = S(x: 1); s.x = 2; return s.x; }",
           R"(struct S { x: int; } struct T { s: S; }
func main(): int { const t = T(s: S(x: 1)); t.s.x += 1; return t.s.x; })",
       }) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(CheckTestSource(source).Succeeded());
  }
}

TEST(DeclarationTest, StructGenericApplicationsRetainValueLayout) {
  auto parsed = ParseTestSource(R"(
struct Pair<T> { first: T; second: T; }
struct Position { x: double; y: double; }
func count(value: Pair<Position>): double { return value.first.x; }
)");
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded());
  const auto& type = checked.model.functions.at("count").parameters.front().type;
  ASSERT_NE(type.value_layout, nullptr);
  ASSERT_EQ(type.value_layout->fields.size(), 2U);
  EXPECT_EQ(type.value_layout->slots.size(), 4U);
}

TEST(DeclarationTest, FieldLookupKeepsPublicNamesAndLexicalPrivateOwnersDistinct) {
  auto parsed = ParseTestSource(R"(
class Base { publicFieldWithALongName: int = 42; #value: int = 1; }
class Child extends Base { #value: int = 2; }
)");
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded());
  const auto& base = checked.model.classes.at("Base");
  const auto& child = checked.model.classes.at("Child");
  const auto* inherited = child.FindField("publicFieldWithALongName");
  const auto* declared = base.FindField("publicFieldWithALongName");
  ASSERT_NE(inherited, nullptr);
  ASSERT_NE(declared, nullptr);
  EXPECT_EQ(inherited->index, declared->index);
  ASSERT_EQ(base.constructor_bindings.size(), 1U);
  ASSERT_EQ(child.constructor_bindings.size(), 1U);
  EXPECT_EQ(base.constructor_bindings.front().field, declared);
  EXPECT_EQ(child.constructor_bindings.front().field, declared);
  const auto* base_private = base.FindField("#value");
  const auto* child_private = child.FindField("#value");
  ASSERT_NE(base_private, nullptr);
  ASSERT_NE(child_private, nullptr);
  EXPECT_EQ(base_private->owner, "Base");
  EXPECT_EQ(child_private->owner, "Child");
  EXPECT_NE(base_private->index, child_private->index);
  EXPECT_EQ(child.FindField("missing"), nullptr);
}

TEST_P(DeclarationErrorTest, RejectsInvalidDeclarationsAndConstWrites) {
  EXPECT_FALSE(CheckTestSource(GetParam()).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    SyntaxAndBindings,
    DeclarationErrorTest,
    testing::Values("int main() { return 1; }",
                    "function main(): int { return 1; }",
                    "func main(int value): int { return value; }",
                    "func main() { return 1; }",
                    "class A { int value; }",
                    "class A { func read(): int { return 1; } }",
                    "class A { public value: int; }",
                    "class A { param value: int; }",
                    "class A { public read(): int { return 1; } }",
                    "class A { private param value: int = 0; }",
                    "class A { public constructor() {} }",
                    "func main(): int { int value = 1; return value; }",
                    "func main(): int { let value; return 1; }",
                    "func main(): int { const value: int; return 1; }",
                    "func main(): int { let value = []; return 1; }",
                    "func nothing(): void {} func main(): int { const x = nothing(); return 1; }",
                    "func main(): int { const value = 1; value = 2; return value; }",
                    "func main(): int { let value = 1; value = 2.0; return value; }",
                    "func main(): int { const value: double = 1; return 1; }",
                    "func main(): int { const values = [1]; values = [2]; return 1; }",
                    "class A { value: int; reset(): void { this = A(value: 1); } }",
                    "func main(): int { for (const i = 0; i < 2; i = i + 1) {} return 1; }"));

TEST(DeclarationTest, AcceptsInferenceContextAndMutableReferences) {
  constexpr std::string_view source = R"(
enum Shade { light, dark }
class Counter {
  value: int;
  add(step: int): int { this.value = this.value + step; return this.value; }
}
func read(value: int): int { return value; }
func main(): int {
  const counter = Counter(value: 1);
  const values = [1, 2];
  const empty: int[] = [];
  const shade: Shade = Shade.light;
  const scale = 1.5;
  let count = 0;
  count = counter.add(read(values[0]));
  values[1] = count;
  { let counter = 2; counter = 3; }
  return values[1];
}
)";
  EXPECT_TRUE(CheckTestSource(source).Succeeded());
}

TEST(DeclarationTest, DuplicateConstructorParametersPreserveForwardingBindingsAndDefaults) {
  for (const std::string_view source : {
           "class Child { y: int = 2; constructor(x: int, x: int, this.y) {} }",
           "class Base { y: int = 2; } class Child extends Base { constructor(x: int, x: int, "
           "super.y) {} }",
       }) {
    auto parsed = ParseTestSource(source);
    ASSERT_TRUE(parsed.diagnostics.empty());
    const auto checked = TypeChecker().Check(parsed.program);
    EXPECT_FALSE(checked.Succeeded());
    const auto& child = checked.model.classes.at("Child");
    ASSERT_EQ(child.constructor.parameters.size(), 2U);
    ASSERT_EQ(child.constructor_bindings.size(), 2U);
    const auto& parameter = child.constructor.parameters[1];
    EXPECT_EQ(parameter.name, "y");
    EXPECT_EQ(parameter.type.kind, TypeDescriptor::Kind::Int);
    EXPECT_TRUE(parameter.has_default);
    EXPECT_TRUE(child.constructor.optional_parameters.contains("y"));
    EXPECT_EQ(child.constructor_bindings[1].declaration,
              &child.declaration->constructor->parameters[2]);
  }
}

class EnumMemberSyntaxTest : public testing::TestWithParam<std::string_view> {};

TEST_P(EnumMemberSyntaxTest, RejectsUnqualifiedEnumMembers) {
  const auto result = CheckTestSource(GetParam());
  EXPECT_FALSE(result.Succeeded());
  ASSERT_FALSE(result.diagnostics.empty());
  EXPECT_EQ(result.diagnostics.front().message, "expected expression");
}

INSTANTIATE_TEST_SUITE_P(
    ExplicitEnumTypes,
    EnumMemberSyntaxTest,
    testing::Values(
        "enum Shade { light } class Theme { shade: Shade; } "
        "func main(): Theme { return Theme(shade: .light); }",
        "enum Shade { light } func main(): Shade { return .light; }",
        "enum Shade { light } func main(): Shade { let value: Shade = .light; return value; }",
        "enum Shade { light } func main(): Shade { const value = .light; return value; }",
        "enum Shade { light } func main(): Shade[] { return [.light]; }",
        "enum Shade { light } func main(): Shade[][] { return [[], [.light]]; }",
        "enum Shade { light } func main(): Shade { "
        "let value = Shade.light; value = .light; return value; }",
        "enum Shade { light } func take(value: Shade): Shade { return value; } "
        "func main(): Shade { return take(.light); }"));

}  // namespace
}  // namespace vela::frontend
