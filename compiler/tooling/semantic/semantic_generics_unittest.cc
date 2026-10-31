#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/modules/source_loader.h"
#include "compiler/tooling/semantic/semantic_index.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {
namespace {

TEST(GenericSemanticTest, UnusedDefinitionsExposeParameterIdentityBoundsAndMembers) {
  const std::string source = R"(
interface Reader<E> { read(): E; }
func use<T extends Reader<int>>(value: T): int { return value.read(); }
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto text = SourceText::Create(source);
  ASSERT_TRUE(text);
  const auto reference = source.find("value: T") + 7;
  const auto definition = index.FindDefinition(reference, PositionEncoding::Utf8);
  if (!definition) {
    ADD_FAILURE() << "missing type parameter definition";
    return;
  }
  EXPECT_EQ(definition.value().range.start,
            text->PositionAt(source.find("use<T") + 4, PositionEncoding::Utf8));
  const auto hover = index.FindHover(reference, PositionEncoding::Utf8);
  if (!hover) {
    ADD_FAILURE() << "missing type parameter hover";
    return;
  }
  EXPECT_EQ(hover.value().signature, "type parameter T extends Reader<int>");
  const auto member = source.find("value.read") + 6;
  const auto target = index.FindDefinition(member, PositionEncoding::Utf8);
  if (!target) {
    ADD_FAILURE() << "missing upper-bound member definition";
    return;
  }
  EXPECT_EQ(target.value().range.start,
            text->PositionAt(source.find("read()"), PositionEncoding::Utf8));
  const auto method = index.FindHover(member, PositionEncoding::Utf8);
  if (!method) {
    ADD_FAILURE() << "missing substituted upper-bound method signature";
    return;
  }
  EXPECT_EQ(method->signature, "(method) Reader<int>.read(): int");
  const auto completions = index.Complete(member);
  EXPECT_NE(std::ranges::find(completions, "read", &Completion::label), completions.end());
  const auto value = index.FindHover(source.find("value.read"), PositionEncoding::Utf8);
  if (!value) {
    ADD_FAILURE() << "missing abstract parameter hover";
    return;
  }
  EXPECT_EQ(value.value().signature, "(parameter) value: T");
}

TEST(GenericSemanticTest, MethodParametersShadowOwnerParametersByDeclarationIdentity) {
  const std::string source = R"(
interface Reader<E> { read(): E; }
class Box<T extends Reader<int>> {
  length<T extends Reader<string>>(value: T): int { return value.read().length; }
}
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto text = SourceText::Create(source);
  ASSERT_TRUE(text);
  const auto reference = source.find("value: T") + 7;
  const auto definition = index.FindDefinition(reference, PositionEncoding::Utf8);
  if (!definition) {
    ADD_FAILURE() << "missing method type parameter definition";
    return;
  }
  EXPECT_EQ(definition.value().range.start,
            text->PositionAt(source.find("length<T") + 7, PositionEncoding::Utf8));
  const auto hover = index.FindHover(reference, PositionEncoding::Utf8);
  if (!hover) {
    ADD_FAILURE() << "missing method type parameter hover";
    return;
  }
  EXPECT_EQ(hover.value().signature, "type parameter T extends Reader<string>");
  const auto member = source.find("value.read") + 6;
  const auto completions = index.Complete(member);
  EXPECT_NE(std::ranges::find(completions, "read", &Completion::label), completions.end());
}

TEST(GenericSemanticTest, BoundCompletionIncludesInheritedMethodsAndMethodTemplates) {
  const std::string source = R"(
class Base { identity<T>(value: T): T { return value; } read(): int { return 42; } }
class Child extends Base {}
func use<T extends Child>(value: T): int { return value.read(); }
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto member = source.find("value.read") + 6;
  const auto completions = index.Complete(member);
  EXPECT_NE(std::ranges::find(completions, "read", &Completion::label), completions.end());
  EXPECT_NE(std::ranges::find(completions, "identity", &Completion::label), completions.end());
}

TEST(GenericSemanticTest, NonNullFactsProjectAcrossDependentNullableBounds) {
  const std::string source = R"(
interface Reader<E> { read(): E; }
func use<T extends U, U extends Reader<int>?>(value: T): int {
  if (value != null) { return value.read(); }
  return 0;
}
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto member = source.find("value.read") + 6;
  const auto completions = index.Complete(member);
  EXPECT_NE(std::ranges::find(completions, "read", &Completion::label), completions.end());
}

TEST(GenericSemanticTest, RenameSeparatesOwnerMethodAndOtherDeclarationParameters) {
  const std::string source = R"(
interface Reader<E> { read(): E; }
class Box<T> {
  value: T;
  read<T extends Reader<int>, U extends T>(value: T, other: U): int {
    return value.read();
  }
}
func identity<T>(value: T): T { return value; }
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto text = SourceText::Create(source);
  if (!text) {
    ADD_FAILURE() << text.error();
    return;
  }
  const auto declaration = source.find("read<T") + 5;
  const auto edits = index.RenameSymbol(declaration, "Element", PositionEncoding::Utf8);
  ASSERT_TRUE(edits) << edits.error();
  ASSERT_EQ(edits->size(), 3U);
  EXPECT_EQ(edits->at(0).range.start, text->PositionAt(declaration, PositionEncoding::Utf8));
  EXPECT_EQ(edits->at(1).range.start,
            text->PositionAt(source.find("U extends T") + 10, PositionEncoding::Utf8));
  EXPECT_EQ(edits->at(2).range.start,
            text->PositionAt(source.find("value: T, other") + 7, PositionEncoding::Utf8));
  const auto prepared =
      index.PrepareRename(source.find("value: T, other") + 7, PositionEncoding::Utf8);
  if (!prepared) {
    ADD_FAILURE() << "missing rename range";
    return;
  }
  EXPECT_EQ(prepared->range.start, edits->at(2).range.start);
}

TEST(GenericSemanticTest, RenameRejectsDuplicateParametersAndReferenceCapture) {
  const auto sources = std::to_array<std::string_view>({
      "func use<T, U>(value: T): T { return value; }",
      "class Box<T> { use<U>(value: T, other: U): T { return value; } }",
      "class U {} func use<T>(value: T, other: U): T { return value; }",
      "func use<T>(value: T, other: U): T { return value; }",
  });
  for (const auto source : sources) {
    SCOPED_TRACE(source);
    const frontend::SourceInput input{.path = "main.vela", .text = source};
    const SemanticIndex index(input.path, std::span(&input, 1));
    const auto renamed = index.RenameSymbol(source.find("<T") + 1, "U", PositionEncoding::Utf8);
    EXPECT_FALSE(renamed);
  }
}

TEST(GenericSemanticTest, RenameRejectsCapturingOwnerReferencesInMethodScope) {
  const std::string source = "class Box<T> { use<U>(value: T, other: U): T { return value; } }";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  EXPECT_FALSE(index.RenameSymbol(source.find("<U") + 1, "T", PositionEncoding::Utf8));
}

TEST(GenericSemanticTest, RenamePreservesKnownFactsWithIncompleteTrailingInput) {
  const std::string source =
      "func identity<T>(value: T): T { return value; }\nfunc incomplete<T extends";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto renamed = index.RenameSymbol(source.find("<T") + 1, "Result", PositionEncoding::Utf8);
  ASSERT_TRUE(renamed) << renamed.error();
  EXPECT_EQ(renamed->size(), 3U);
  EXPECT_TRUE(index.PrepareRename(source.find("value;"), PositionEncoding::Utf8));
  for (const std::string_view name : {"class", "T,U", "", " T", "T/*comment*/"}) {
    EXPECT_FALSE(index.RenameSymbol(source.find("<T") + 1, name, PositionEncoding::Utf8));
  }
}

}  // namespace
}  // namespace vela::tooling
