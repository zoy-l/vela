#include <array>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "compiler/frontend/source_loader.h"
#include "compiler/tooling/semantic_index.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {
namespace {

TEST(MapLiteralSemanticTest, VariableKeysRetainSymbolsAndStringKeysRemainData) {
  const std::string source =
      "func make(key: string, value: int): Map<string,int> { return {key: value, 'key': 2}; }";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto text = SourceText::Create(source);
  ASSERT_TRUE(text);
  const auto key = source.find("{key") + 1;
  const auto definition = index.FindDefinition(key, PositionEncoding::Utf8);
  if (!definition) {
    ADD_FAILURE() << "missing variable key definition";
    return;
  }
  EXPECT_EQ(definition->range.start, text->PositionAt(source.find("key:"), PositionEncoding::Utf8));
  EXPECT_FALSE(index.FindDefinition(source.find("'key'") + 1, PositionEncoding::Utf8));
  EXPECT_FALSE(index.PrepareRename(source.find("'key'") + 1, PositionEncoding::Utf8));
  const auto edits = index.RenameSymbol(key, "label", PositionEncoding::Utf8);
  ASSERT_TRUE(edits) << edits.error();
  EXPECT_EQ(edits->size(), 2U);
  EXPECT_FALSE(index.RenameSymbol(key, "value", PositionEncoding::Utf8));
  const auto hover = index.FindHover(source.find(": value") + 2, PositionEncoding::Utf8);
  if (!hover) {
    ADD_FAILURE() << "missing map value hover";
    return;
  }
  EXPECT_TRUE(hover->signature.contains("value: int"));
}

TEST(MapLiteralSemanticTest, RenamesGlobalKeysAcrossModulesAndKeepsIncompleteEntryReferences) {
  const std::array sources{
      frontend::SourceInput{
          .path = "main.vela",
          .text = "import 'keys.vela'; func main(): void { const m = {key: 42, 'key': 1}; }",
      },
      frontend::SourceInput{.path = "keys.vela", .text = "export let key = 'answer';"},
  };
  const SemanticIndex index(sources.front().path, sources);
  const auto key = sources.front().text.find("{key") + 1;
  const auto edits = index.RenameSymbol(key, "label", PositionEncoding::Utf8);
  ASSERT_TRUE(edits) << edits.error();
  ASSERT_EQ(edits->size(), 2U);
  EXPECT_NE(edits->front().path, edits->back().path);
  const frontend::SourceInput incomplete{
      .path = "main.vela",
      .text = "func main(key: string): void { const m = {key:",
  };
  const SemanticIndex partial(incomplete.path, std::span(&incomplete, 1));
  EXPECT_TRUE(partial.FindDefinition(incomplete.text.find("{key") + 1, PositionEncoding::Utf8));
}

}  // namespace
}  // namespace vela::tooling
