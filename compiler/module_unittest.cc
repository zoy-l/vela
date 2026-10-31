#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/checker_test_support.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/program_test_support.h"
#include "runtime/isolate.h"

namespace vela::compiler {
namespace {

TEST(ModuleTest, RejectsUnsupportedImportSchemes) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import \"registry:math/value.vela\";"},
  });
  const auto checked = frontend::CheckTestModules(sources, "main.vela");
  ASSERT_FALSE(checked.Succeeded());
  ASSERT_EQ(checked.diagnostics.size(), 1U);
  EXPECT_EQ(checked.diagnostics.front().message, "unsupported module import scheme");
}

TEST(ModuleTest, CachedGenericSyntaxSurvivesSpecializationInIndependentRoots) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "first.vela", .text = "import 'middle'; func first(): int { return number(); }"},
      {
          .path = "middle.vela",
          .text = "import 'shared'; export func number(): int { return identity<int>(42); }",
      },
      {
          .path = "second.vela",
          .text = "import 'shared'; func second(): string { return identity<string>('second'); }",
      },
      {.path = "shared.vela", .text = "export func identity<T>(value: T): T { return value; }"},
  });
  frontend::ModuleCache cache;
  auto first = frontend::LoadProgram("first.vela", cache, sources);
  ASSERT_TRUE(first.diagnostics.empty());
  ASSERT_TRUE(frontend::CheckProgram(first.program).Succeeded());
  const auto& path = first.program.sources.at(2).path;
  const auto syntax = cache.sources.at(path);
  const auto& generic = syntax->program.functions.front();
  EXPECT_EQ(generic.name, "identity");
  ASSERT_EQ(generic.type_parameters.size(), 1U);
  EXPECT_EQ(generic.return_type.name, "T");
  auto second = frontend::LoadProgram("second.vela", cache, sources);
  ASSERT_TRUE(second.diagnostics.empty());
  EXPECT_EQ(first.program.sources.at(2).syntax, second.program.sources.at(1).syntax);
  EXPECT_TRUE(frontend::CheckProgram(second.program).Succeeded());
  EXPECT_EQ(cache.sources.at(path), syntax);
  EXPECT_EQ(generic.name, "identity");
  EXPECT_EQ(generic.return_type.name, "T");
  EXPECT_EQ(generic.body->statements.front()->expression->text, "value");
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
  const auto compiled = test::CompileCleanModules(sources, "app/main.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 41);
}

TEST(ModuleTest, DoesNotSearchForExtensionlessFilesOrDirectoryEntryPoints) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'lib';"},
      {.path = "lib", .text = "export const answer = 42;"},
      {.path = "lib/index.vela", .text = "export const answer = 42;"},
  });
  EXPECT_FALSE(frontend::CheckTestModules(sources, "main.vela").Succeeded());
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
export class Box<T> { value: T; get(): T { return this.value; } }
)",
      },
      {
          .path = "right.vela",
          .text = R"(
class Hidden { value: int; }
export type Box = Hidden;
export func read(value: Box): int { return value.value; }
)",
      },
  });
  EXPECT_TRUE(frontend::CheckTestModules(sources, "main.vela").Succeeded());
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
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
  const auto compiled = test::CompileCleanModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(ModuleTest, LocalVariablesShadowImportPrefixesAndImportedFunctions) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import "lib.vela" as lib;
import "lib.vela";
class Local { value: int; }
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
  const auto compiled = test::CompileCleanModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(ModuleTest, BindsImportedGenericBasesAndIterablesBeforeLoopShadowing) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import 'derived.vela';
func main(): int { return Derived<int>(value: 20).read(); }
)",
      },
      {
          .path = "base.vela",
          .text = R"(
export class Base<T extends int> { stored: T; read(): int { return this.stored; } }
export func values<T>(value: T): T[] { return [value, value]; }
)",
      },
      {
          .path = "derived.vela",
          .text = R"(
import 'base.vela' as lib;
export class Derived<T extends int> extends lib.Base<T> {
  constructor(value: T) { super(stored: value); }
  override read(): int {
    let result = super.read();
    for (const lib in lib.values<T>(this.stored)) {
      const capture = (): T => lib;
      result += capture() / 2;
    }
    return result + 2;
  }
}
)",
      },
  });
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(ModuleTest, SharesImmutableSyntaxAcrossRootsWithIndependentBindingsAndLocations) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "first.vela", .text = "import 'middle'; func first(): int { return read(); }"},
      {
          .path = "middle.vela",
          .text = "import 'shared'; export func read(): int { return value(); }",
      },
      {.path = "second.vela", .text = "import 'shared'; func second(): int { return value(); }"},
      {
          .path = "shared.vela",
          .text = "export func value(): int { return Math.sqrt(1764.0).toInt(); }",
      },
  });
  frontend::ModuleCache cache;
  auto first = frontend::LoadProgram("first.vela", cache, sources);
  ASSERT_TRUE(first.diagnostics.empty());
  const auto second = frontend::LoadProgram("second.vela", cache, sources);
  ASSERT_TRUE(second.diagnostics.empty());
  const auto& first_shared = first.program.sources.at(2);
  const auto& second_shared = second.program.sources.at(1);
  EXPECT_EQ(first_shared.syntax, second_shared.syntax);
  EXPECT_EQ(first_shared.text.data(), second_shared.text.data());
  const auto& shared_template = cache.sources.at(first_shared.path)->program;
  EXPECT_EQ(shared_template.functions.front().name, "value");
  const auto& first_value = first.program.functions.at(2);
  const auto& second_value = second.program.functions.at(1);
  EXPECT_EQ(first_value.location.source_id, 2U);
  EXPECT_EQ(second_value.location.source_id, 1U);
  EXPECT_EQ(second_value.body->statements.front()->expression->location.source_id, 1U);
  first.program.functions.at(2).name = "changed";
  EXPECT_NE(second_value.name, "changed");
  EXPECT_EQ(shared_template.functions.front().name, "value");
}

TEST(ModuleTest, TransferredModulesRetainSourceTextTokensAndOriginalImports) {
  std::vector<frontend::SourceFile> retained;
  const std::string expected = "import 'lib'; func main(): int { return value(); }";
  {
    const std::string& main = expected;
    const std::string library = "export func value(): int { return 42; }";
    const auto sources = std::to_array<frontend::SourceInput>({
        {.path = "main.vela", .text = main},
        {.path = "lib.vela", .text = library},
    });
    auto loaded = frontend::LoadSources(sources, "main.vela");
    ASSERT_TRUE(loaded.diagnostics.empty());
    retained = std::move(loaded.program.sources);
  }
  ASSERT_EQ(retained.size(), 2U);
  const auto& main = retained.front();
  EXPECT_EQ(main.text, expected);
  EXPECT_EQ(main.text.data(), main.syntax->text.data());
  ASSERT_EQ(main.syntax->imports.size(), 1U);
  EXPECT_EQ(main.syntax->imports.front().path, "lib");
  EXPECT_FALSE(main.syntax->tokens.empty());
  ASSERT_FALSE(retained.back().syntax->tokens.empty());
  EXPECT_EQ(retained.back().syntax->tokens.front().location.source_id, 1U);
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
class Hidden { value: int; }
export func visible(): int { return leaf(); }
)",
      },
      {.path = "leaf.vela", .text = "export func leaf(): int { return 42; }"},
  });
  EXPECT_FALSE(frontend::CheckTestModules(sources, "main.vela").Succeeded());
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
class Secret { value: int; }
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
