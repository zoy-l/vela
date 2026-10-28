#include "gtest/gtest.h"

#include "compiler/tooling/documentation.h"

namespace vela::tooling {
namespace {

TEST(DocumentationTest, EmitsOnlyExportsInSourceOrderAndPreservesDocComments) {
  const auto result = GenerateModuleDocumentation(R"(
/// Shared value.
export const answer: int = 42;
const hidden: int = 0;
/// Adds two values.
/// Both inputs are strict integers.
export func add(left: int, right: int): int { return left + right; }
func privateFunction(): void {}
)");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->contains("Shared value."));
  EXPECT_TRUE(result->contains("Both inputs are strict integers."));
  EXPECT_TRUE(result->contains("func add(left: int, right: int): int;"));
  EXPECT_LT(result->find("## `answer`"), result->find("## `add`"));
  EXPECT_FALSE(result->contains("hidden"));
  EXPECT_FALSE(result->contains("privateFunction"));
}

TEST(DocumentationTest, DescribesClassVisibilityConstructorDefaultsAndMethods) {
  const auto result = GenerateModuleDocumentation(R"(
/// A counter.
export class Counter<T> {
  /// Initial value.
  value: T;
  private secret: int = 1;
  static count: int = 0;
  private internal: int = 0;
  constructor { this.internal = this.secret; }
  /// Read the stored value.
  read(): T { return this.value; }
  private reset(): void {}
}
)");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->contains("class Counter<T>"));
  EXPECT_TRUE(result->contains("Counter(value: T);"));
  EXPECT_FALSE(result->contains("secret"));
  EXPECT_FALSE(result->contains("public "));
  EXPECT_FALSE(result->contains("param "));
  EXPECT_TRUE(result->contains("Initial value."));
  EXPECT_TRUE(result->contains("Read the stored value."));
  EXPECT_TRUE(result->contains("read(): T;"));
  EXPECT_FALSE(result->contains("internal"));
  EXPECT_FALSE(result->contains("reset"));
}

TEST(DocumentationTest, RendersNullableArraysGenericsFunctionsInterfacesAndEnums) {
  const auto result = GenerateModuleDocumentation(R"(
export type Operation = (int, string[]) => Map<string, int?[]>;
export enum Mode { light, dark, }
export interface Reader { read(): int[]?[]; }
export class Box implements Reader { read(): int[]?[] { return []; } }
)");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->contains("type Operation = (int, string[]) => Map<string, int?[]>;"));
  EXPECT_TRUE(result->contains("enum Mode {\n  light,\n  dark,"));
  EXPECT_TRUE(result->contains("read(): int[]?[];"));
  EXPECT_TRUE(result->contains("class Box implements Reader"));
}

TEST(DocumentationTest, ExplicitConstructorsOwnSignaturesAndPrivateConstructorsAreNotCallable) {
  const auto result = GenerateModuleDocumentation(R"(
export class Box {
  value: int;
  label: string = '';
  constructor(initial: int) { this.value = initial; }
}
export class Hidden { private constructor() {} }
export class GeneratedHidden { value: int; private constructor {} }
)");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->contains("constructor(initial: int) { … }"));
  EXPECT_TRUE(result->contains("Box(initial: int);"));
  EXPECT_FALSE(result->contains("Box(value:"));
  EXPECT_FALSE(result->contains("named constructor parameter"));
  EXPECT_TRUE(result->contains("private constructor() { … }"));
  EXPECT_TRUE(result->contains("private constructor { … }"));
  EXPECT_FALSE(result->contains("Hidden();"));
  EXPECT_FALSE(result->contains("GeneratedHidden(value:"));
}

TEST(DocumentationTest, DocumentsEmptyPublicSurfaceAndRejectsSyntaxErrors) {
  const auto empty = GenerateModuleDocumentation("func main(): void {}");
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->contains("no exported declarations"));
  EXPECT_FALSE(GenerateModuleDocumentation("export func broken("));
}

}  // namespace
}  // namespace vela::tooling
