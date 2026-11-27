#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/tooling/semantic/semantic_index.h"

namespace vela::tooling {
namespace {

TEST(SemanticUnusedTest,
     TracksParameterReadsThroughDefaultsCapturesShadowingAndGenericDefinitions) {
  const std::string source = R"(
interface Reader { read(value: int): int; }
external func host(value: int): void;
func captured(value: int): () => int { return () => value; }
func defaults(first: int, second: int = first): int { return second; }
func ignored(_value: int): void {}
func written(value: int): void { value = 1; }
func shadowed(value: int): void { { const value = 1; print(value); } }
func generic<T>(value: T): void {}
func named(value: int): void {}
func main(): void { named(value: 1); }
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto diagnostics = index.diagnostics();
  constexpr std::array<std::uint32_t, 4> kExpectedLines{7, 8, 9, 10};
  ASSERT_EQ(diagnostics.size(), kExpectedLines.size());
  for (std::size_t position = 0; position < diagnostics.size(); ++position) {
    const auto& diagnostic = diagnostics[position];
    EXPECT_EQ(diagnostic.code, frontend::DiagnosticCode::UnusedParameter);
    EXPECT_EQ(diagnostic.severity, frontend::DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.location.line, kExpectedLines[position]);
    EXPECT_EQ(diagnostic.length, 5U);
  }
}

TEST(SemanticUnusedTest, CountsOnlyThisModulesBoundTypeAndValueReferencesForEachImport) {
  const std::string source = R"(import 'library' as used;
import 'library' as unused;
import 'library';
import 'types';
import 'spare';
class Holder { value: used.Item?; }
func identity<T>(value: Box<T>): Box<T> { return value; }
func main(): void { const unused = 1; print(unused); }
)";
  const auto inputs = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = source},
      {.path = "library.vela", .text = "export class Item {}"},
      {.path = "types.vela", .text = "export class Box<T> { value: T; }"},
      {.path = "spare.vela", .text = "export func execute(): void {}"},
  });
  const SemanticIndex index(frontend::LoadSources(inputs, "main.vela"));
  const auto diagnostics = index.diagnostics();
  constexpr std::array<std::uint32_t, 3> kExpectedLines{2, 3, 5};
  ASSERT_EQ(diagnostics.size(), kExpectedLines.size());
  for (std::size_t position = 0; position < diagnostics.size(); ++position) {
    const auto& diagnostic = diagnostics[position];
    EXPECT_EQ(diagnostic.code, frontend::DiagnosticCode::UnusedImport);
    EXPECT_EQ(diagnostic.severity, frontend::DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.location.line, kExpectedLines[position]);
    EXPECT_EQ(diagnostic.location.column, 1U);
    EXPECT_GT(diagnostic.length, 7U);
  }
}

TEST(SemanticUnusedTest, SuppressesUnusedReportsUntilInvalidReferencesCanBeResolved) {
  const frontend::SourceInput input{
      .path = "main.vela",
      .text = "func incomplete(value: int): void { missing(); }",
  };
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto diagnostics = index.diagnostics();
  EXPECT_TRUE(frontend::HasErrors(diagnostics));
  for (const auto& diagnostic : diagnostics) {
    EXPECT_NE(diagnostic.code, frontend::DiagnosticCode::UnusedImport);
    EXPECT_NE(diagnostic.code, frontend::DiagnosticCode::UnusedParameter);
  }
}

}  // namespace
}  // namespace vela::tooling
