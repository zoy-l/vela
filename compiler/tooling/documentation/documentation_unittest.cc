#include "compiler/frontend/diagnostic.h"
#include "gtest/gtest.h"

#include <fstream>
#include <string>
#include <string_view>

#include "base/path.h"
#include "base/testing/directory.h"
#include "compiler/tooling/documentation/documentation.h"

namespace vela::tooling {
namespace {

class DocumentationTest : public test::DirectoryTest {
 protected:
  DocumentationResult Generate(std::string_view source) {
    const auto path = directory() / "module.vela";
    {
      std::ofstream output(path);
      output << source;
    }
    return GenerateDocumentation(PathText(path));
  }
};

TEST_F(DocumentationTest, PreservesMapKeyValueSourceWithoutEvaluatingInitializers) {
  const auto result = Generate(R"(
func value(): int { throw Error(message: 'documentation must not execute'); }
export const data: Map<string, int> = {'answer': value()};
export class Box { items: Map<string, Map<string, int>> = {'inner': {'answer': 42}}; }
)");
  ASSERT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
  EXPECT_TRUE(result.markdown.contains("data: Map<string, int> = {'answer': value()};"));
  EXPECT_TRUE(result.markdown.contains(
      "items: Map<string, Map<string, int>> = {'inner': {'answer': 42}};"));
}

TEST_F(DocumentationTest, PreservesAttributeDeclarationsDataAndAttachedComments) {
  const auto result = Generate(R"(
/// Labels a reflected action.
export class Button extends Reflect { label: string = ''; }
/// A reflected counter.
[Reflect]
export class Counter {
  /// Current count.
  [Reflect]
  value: int = 0;
  /// Resets the counter.
  [Button(label: 'reset')]
  reset(): void { this.value = 0; }
}
)");
  ASSERT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
  EXPECT_TRUE(result.markdown.contains("class Button extends Reflect"));
  EXPECT_TRUE(result.markdown.contains("[Reflect]\nclass Counter"));
  EXPECT_TRUE(result.markdown.contains("[Button(label: 'reset')]\nreset(): void;"));
  EXPECT_TRUE(result.markdown.contains("A reflected counter."));
  EXPECT_TRUE(result.markdown.contains("Current count."));
  EXPECT_TRUE(result.markdown.contains("Resets the counter."));
}

TEST_F(DocumentationTest, EmitsOnlyExportsInSourceOrderAndPreservesDocComments) {
  const auto result = Generate(R"(
/// Shared value.
export const answer: int = 42;
const hidden: int = 0;
/// Adds two values.
/// Both inputs are strict integers.
export func add(left: int, right: int): int { return left + right; }
func privateFunction(): void {}
)");
  ASSERT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
  EXPECT_TRUE(result.markdown.contains("Shared value."));
  EXPECT_TRUE(result.markdown.contains("Both inputs are strict integers."));
  EXPECT_TRUE(result.markdown.contains("func add(left: int, right: int): int;"));
  EXPECT_LT(result.markdown.find("## `answer`"), result.markdown.find("## `add`"));
  EXPECT_FALSE(result.markdown.contains("hidden"));
  EXPECT_FALSE(result.markdown.contains("privateFunction"));
}

TEST_F(DocumentationTest, DescribesClassVisibilityConstructorDefaultsAndMethods) {
  const auto result = Generate(R"(
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
  ASSERT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
  EXPECT_TRUE(result.markdown.contains("class Counter<T>"));
  EXPECT_TRUE(result.markdown.contains("Counter(value: T);"));
  EXPECT_FALSE(result.markdown.contains("secret"));
  EXPECT_FALSE(result.markdown.contains("public "));
  EXPECT_FALSE(result.markdown.contains("param "));
  EXPECT_TRUE(result.markdown.contains("Initial value."));
  EXPECT_TRUE(result.markdown.contains("Read the stored value."));
  EXPECT_TRUE(result.markdown.contains("read(): T;"));
  EXPECT_FALSE(result.markdown.contains("internal"));
  EXPECT_FALSE(result.markdown.contains("reset"));
}

TEST_F(DocumentationTest, RendersNullableArraysGenericsFunctionsInterfacesAndEnums) {
  const auto result = Generate(R"(
export type Operation = (int, string[]) => Map<string, int?[]>;
export enum Mode { light, dark, }
export interface Reader { read(): int[]?[]; }
export class Box implements Reader { read(): int[]?[] { return []; } }
)");
  ASSERT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
  EXPECT_TRUE(result.markdown.contains("type Operation = (int, string[]) => Map<string, int?[]>;"));
  EXPECT_TRUE(result.markdown.contains("enum Mode {\n  light,\n  dark,"));
  EXPECT_TRUE(result.markdown.contains("read(): int[]?[];"));
  EXPECT_TRUE(result.markdown.contains("class Box implements Reader"));
}

TEST_F(DocumentationTest, ExplicitConstructorsOwnSignaturesAndPrivateConstructorsAreNotCallable) {
  const auto result = Generate(R"(
export class Box {
  value: int;
  label: string = '';
  constructor(initial: int) { this.value = initial; }
}
export class Hidden { private constructor() {} }
export class GeneratedHidden { value: int; private constructor {} }
)");
  ASSERT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
  EXPECT_TRUE(result.markdown.contains("constructor(initial: int) { … }"));
  EXPECT_TRUE(result.markdown.contains("Box(initial: int);"));
  EXPECT_FALSE(result.markdown.contains("Box(value:"));
  EXPECT_FALSE(result.markdown.contains("named constructor parameter"));
  EXPECT_TRUE(result.markdown.contains("private constructor() { … }"));
  EXPECT_TRUE(result.markdown.contains("private constructor { … }"));
  EXPECT_FALSE(result.markdown.contains("Hidden();"));
  EXPECT_FALSE(result.markdown.contains("GeneratedHidden(value:"));
}

TEST_F(DocumentationTest, GroupsFunctionTypesBeforeArrayAndNullableSuffixes) {
  const auto result = Generate(R"(
export type Operations = ((int) => string)[]?;
export type Nested = Map<string, ((int[]) => string?)?>;
)");
  ASSERT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
  EXPECT_TRUE(result.markdown.contains("type Operations = ((int) => string)[]?;"));
  EXPECT_TRUE(result.markdown.contains("type Nested = Map<string, ((int[]) => string?)?>;"));
}

TEST_F(DocumentationTest, PreservesBoundsOwnerParametersAndMethodParameters) {
  const auto result = Generate(R"(
export interface Reader<T> { read<U extends Map<string, Future<T[]>>>(value: U): T; }
export class Box<T extends Reader<int>> {
  value: T;
  apply<U extends T>(value: U): U { return value; }
}
export type Values<T extends Reader<int>> = Map<string, Future<T[]>>;
export func read<T extends Reader<E>, E>(value: T): E { throw Error(message: 'documentation must not execute'); }
)");
  ASSERT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
  EXPECT_TRUE(result.markdown.contains("interface Reader<T>"));
  EXPECT_TRUE(result.markdown.contains("read<U extends Map<string, Future<T[]>>>(value: U): T;"));
  EXPECT_TRUE(result.markdown.contains("class Box<T extends Reader<int>>"));
  EXPECT_TRUE(result.markdown.contains("apply<U extends T>(value: U): U;"));
  EXPECT_TRUE(
      result.markdown.contains("type Values<T extends Reader<int>> = Map<string, Future<T[]>>;"));
  EXPECT_TRUE(result.markdown.contains("func read<T extends Reader<E>, E>(value: T): E;"));
}

TEST_F(DocumentationTest, DocumentsEmptyPublicSurfaceAndRejectsSyntaxErrors) {
  const auto empty = Generate("func main(): void {}");
  ASSERT_TRUE(empty.Succeeded()) << frontend::DiagnosticsText(empty.diagnostics);
  EXPECT_TRUE(empty.markdown.contains("no exported declarations"));
  EXPECT_FALSE(Generate("export func broken(").Succeeded());
}

}  // namespace
}  // namespace vela::tooling
