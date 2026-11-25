#include <array>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/tooling/semantic/semantic_index.h"
#include "compiler/tooling/semantic/semantic_references.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {
namespace {

TEST(SemanticReferenceTest, DistinguishesShadowedBindingsAndIncludesWritesAndNamedArguments) {
  const std::string source = R"(func consume(value: int): int {
  value = 2;
  { const value = 3; print(value); }
  return value;
}
func main(): int { return consume(value: 1); }
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  ASSERT_FALSE(frontend::HasErrors(index.diagnostics()));
  const auto offset = source.find("value") + 5;
  const auto definition = index.FindDefinition(offset, PositionEncoding::Utf16);
  ASSERT_TRUE(definition);
  ReferenceIndex references_index;
  index.AddReferences(references_index, PositionEncoding::Utf16);
  const auto references =
      references_index.FindReferences(*definition, ReferenceDeclaration::Exclude);
  ASSERT_EQ(references.size(), 3U);
  EXPECT_EQ(references[0].range.start.line, 1U);
  EXPECT_EQ(references[1].range.start.line, 3U);
  EXPECT_EQ(references[2].range.start.line, 5U);
  EXPECT_EQ(references_index.FindReferences(*definition, ReferenceDeclaration::Include).size(), 4U);
}

TEST(SemanticReferenceTest, KeepsSourceTypeIdentityAcrossImportedGenericApplications) {
  const std::string entry =
      "import 'library' as lib;\n"
      "func make(): lib.Item<int> { return lib.Item<int>(value: 1); }\n";
  const std::string library = "export class Item<T> { value: T; }";
  const auto inputs = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = entry},
      {.path = "library.vela", .text = library},
  });
  const SemanticIndex index(frontend::LoadSources(inputs, "main.vela"));
  ASSERT_FALSE(frontend::HasErrors(index.diagnostics()));
  const SemanticIndex library_index(frontend::LoadSources(inputs, "library.vela"));
  const auto definition = index.FindDefinition(entry.find("Item"), PositionEncoding::Utf8);
  ASSERT_TRUE(definition);
  ReferenceIndex references_index;
  index.AddReferences(references_index, PositionEncoding::Utf8);
  library_index.AddReferences(references_index, PositionEncoding::Utf8);
  const auto references =
      references_index.FindReferences(*definition, ReferenceDeclaration::Include);
  ASSERT_EQ(references.size(), 3U);
  EXPECT_EQ(references[0].path, definition->path);
  EXPECT_EQ(references[1].range.start.line, 1U);
  EXPECT_EQ(references[2].range.start.line, 1U);
}

TEST(SemanticReferenceTest, ConnectsOverridesToBaseAndInterfaceCallSites) {
  const std::string source = R"(interface Reader { read(value: int): int; }
class Base implements Reader { read(value: int): int { return value; } }
class Child extends Base { override read(value: int): int { return value + 1; } }
class Other { read(value: int): int { return value; } }
func first(value: Reader): int { return value.read(1); }
func second(value: Base): int { return value.read(2); }
func third(value: Child): int { return value.read(3); }
func unrelated(value: Other): int { return value.read(4); }
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  ASSERT_FALSE(frontend::HasErrors(index.diagnostics()));
  const auto offset = source.find("override read") + std::string("override ").size();
  const auto definition = index.FindDefinition(offset, PositionEncoding::Utf16);
  ASSERT_TRUE(definition);
  ReferenceIndex references_index;
  index.AddReferences(references_index, PositionEncoding::Utf16);
  const auto references =
      references_index.FindReferences(*definition, ReferenceDeclaration::Exclude);
  ASSERT_EQ(references.size(), 3U);
  EXPECT_EQ(references[0].range.start.line, 4U);
  EXPECT_EQ(references[1].range.start.line, 5U);
  EXPECT_EQ(references[2].range.start.line, 6U);
  EXPECT_EQ(references_index.FindReferences(*definition, ReferenceDeclaration::Include).size(), 6U);
}

TEST(SemanticReferenceTest, AbstractOverridesUseTheCheckersContractEdges) {
  const std::string source = R"(class Base<T> {
  read(value: int): int { return value; }
}
class Child<T> extends Base<T> {
  override read(value: int): int { return value + 1; }
}
func use(value: Base<int>): int { return value.read(1); }
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  ASSERT_FALSE(frontend::HasErrors(index.diagnostics()))
      << frontend::DiagnosticsText(index.diagnostics());
  const auto definition = index.FindDefinition(
      source.find("override read") + std::string("override ").size(), PositionEncoding::Utf16);
  ASSERT_TRUE(definition);
  ReferenceIndex references;
  index.AddReferences(references, PositionEncoding::Utf16);
  const auto occurrences = references.FindReferences(*definition, ReferenceDeclaration::Include);
  ASSERT_EQ(occurrences.size(), 3U);
  EXPECT_EQ(occurrences.back().range.start.line, 6U);
}

}  // namespace
}  // namespace vela::tooling
