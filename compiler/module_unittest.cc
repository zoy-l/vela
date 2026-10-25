#include <array>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/source_loader.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(ModuleTest, RejectsUnsupportedImportSchemes) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import \"registry:math/value.vela\";"},
  });
  const auto checked = frontend::CheckModules(sources, "main.vela");
  ASSERT_FALSE(checked.Succeeded());
  ASSERT_EQ(checked.diagnostics.size(), 1U);
  EXPECT_EQ(checked.diagnostics.front().message, "unsupported module import scheme");
}

TEST(ModuleTest, ResolvesExtensionlessImportsToVelaAndSharesIdentityWithExplicitImports) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "app/main.vela",
          .text = "import './lib'; import './nested/../lib.vela' as lib; func main(): int { return "
                  "next() + lib.next(); }",
      },
      {
          .path = "app/lib.vela",
          .text = "let value = 19; export func next(): int { value = value + 1; return value; }",
      },
      {.path = "app/lib", .text = "this is not a Vela module"},
  });
  runtime::Isolate isolate;
  const auto compiled = CompileModules(sources, "app/main.vela", isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 41);
}

TEST(ModuleTest, DoesNotSearchForExtensionlessFilesOrDirectoryEntryPoints) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'lib';"},
      {.path = "lib", .text = "export const answer = 42;"},
      {.path = "lib/index.vela", .text = "export const answer = 42;"},
  });
  EXPECT_FALSE(frontend::CheckModules(sources, "main.vela").Succeeded());
}

TEST(ModuleTest, PrefixesKeepSameNamesDistinctAcrossFunctionsClassesEnumsAndAliases) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import "left.vela" as left;
import "right.vela" as right;
func main(): int {
  const a: left.Box<int> = left.Box<int>(value: left.identity<int>(20));
  const b: right.Box = right.Box(value: 21);
  const color: left.Color = left.Color.light;
  const bound: () => int = a.get;
  if (color == left.Color.light) { return bound() + right.read(b) + 1; }
  return 0;
}
)",
      },
      {
          .path = "left.vela",
          .text = R"(
export func identity<T>(value: T): T { return value; }
export enum Color { light, dark }
export class Box<T> { param value: T; get(): T { return this.value; } }
)",
      },
      {
          .path = "right.vela",
          .text = R"(
class Hidden { param value: int; }
export type Box = Hidden;
export func read(value: Box): int { return value.value; }
)",
      },
  });
  EXPECT_TRUE(frontend::CheckModules(sources, "main.vela").Succeeded());
  runtime::Isolate isolate(1);
  const auto compiled = CompileModules(sources, "main.vela", isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.source_path << ":" << diagnostic.location.line << ": "
                  << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(ModuleTest, ClosuresAndFunctionCyclesResolveTheirDefiningModule) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import "counter.vela" as lib;
export func stop(): int { return 40; }
func main(): int { const f = lib.make(); f(); return f(); }
)",
      },
      {
          .path = "counter.vela",
          .text = R"(
import "main.vela" as main;
func start(): int { return main.stop(); }
export func make(): () => int {
  let value: int = start();
  return (): int => { value = value + 1; return value; };
}
)",
      },
  });
  runtime::Isolate isolate(1);
  const auto compiled = CompileModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(ModuleTest, LocalVariablesShadowImportPrefixesAndImportedFunctions) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import "lib.vela" as lib;
import "lib.vela";
class Local { param value: int; }
func main(): int {
  const lib = Local(value: 40);
  const value = (input: int): int => input + 1;
  return value(lib.value) + 1;
}
)",
      },
      {.path = "lib.vela", .text = "export func value(input: int): int { return input; }"},
  });
  runtime::Isolate isolate;
  const auto compiled = CompileModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

class ModuleVisibilityTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ModuleVisibilityTest, RejectsAccessOutsideTheExplicitModuleScope) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = GetParam()},
      {
          .path = "lib.vela",
          .text = R"(
import "leaf.vela";
func hidden(): int { return 42; }
class Hidden { param value: int; }
export func visible(): int { return leaf(); }
)",
      },
      {.path = "leaf.vela", .text = "export func leaf(): int { return 42; }"},
  });
  EXPECT_FALSE(frontend::CheckModules(sources, "main.vela").Succeeded());
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileModules(sources, "main.vela", isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    ExportBoundaries,
    ModuleVisibilityTest,
    testing::Values(
        "import \"lib.vela\"; func main(): int { return hidden(); }",
        "import \"lib.vela\"; func main(): int { return leaf(); }",
        "import \"lib.vela\" as lib; func main(): int { return lib.hidden(); }",
        "import \"lib.vela\" as lib; func main(): lib.Hidden { return lib.Hidden(value: 1); }",
        "import \"lib.vela\" as lib; func main(): int { return visible(); }",
        "import \"lib.vela\" as lib; func main(): void { const value = lib; }",
        "import \"lib.vela\" as lib; func lib(): void {}"));

TEST(ModuleTest, DependenciesCannotReadPrivateEntrySymbolsThroughTheLinkedProgram) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import "lib.vela";
func secret(): int { return 42; }
class Secret { param value: int; }
func main(): int { return read(); }
)",
      },
      {.path = "lib.vela", .text = "export func read(): int { return secret(); }"},
  });
  runtime::Isolate isolate;
  const auto compiled = CompileModules(sources, "main.vela", isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_EQ(compiled.diagnostics.front().source_path, "lib.vela");
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("secret"));
}

TEST(ModuleTest, TypeParametersShadowImportPrefixesInQualifiedTypes) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import "lib.vela" as lib;
func invalid<lib>(value: lib.Item): int { return 0; }
func main(): int { return invalid<int>(1); }
)",
      },
      {.path = "lib.vela", .text = "export type Item = int;"},
  });
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileModules(sources, "main.vela", isolate).Succeeded());
}

TEST(ModuleTest, ConflictingExportsRequirePrefixes) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(import "a.vela"; import "b.vela"; func main(): int { return f(); })",
      },
      {.path = "a.vela", .text = "export func f(): int { return 1; }"},
      {.path = "b.vela", .text = "export func f(): int { return 2; }"},
  });
  runtime::Isolate isolate;
  const auto compiled = CompileModules(sources, "main.vela", isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("conflicting imported name"));
}

TEST(ModuleTest, UnsuppliedMemoryImportsDoNotReadTheDisk) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("import \"examples/imports/math.vela\";", isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("was not supplied"));
}

}  // namespace
}  // namespace vela::compiler
