#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/tooling/semantic/semantic_index.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {
namespace {

TEST(SemanticCompletionTest, GlobalReflectionDeclarationsAreDiscoverableWithoutAnImport) {
  const std::string source = "func main(): void { Reflect.typeI; }";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto completions = index.Complete(source.find("typeI") + 5);
  const auto function = std::ranges::find(completions, "typeInfo", &Completion::label);
  ASSERT_NE(function, completions.end());
  EXPECT_EQ(function->kind, CompletionKind::Method);
  EXPECT_NE(function->detail.find("<T>"), std::string::npos);
  EXPECT_FALSE(function->documentation.empty());
}

TEST(SemanticAttributeTest, ResolvesAttributeTypesArgumentsAndCommentsThroughNormalSymbols) {
  const std::string source = R"(
/// Labels a method.
class Button extends Reflect { label: string = ''; }
class Counter {
  /// Runs the action.
  [Button(label: 'run')]
  run(): void {}
}
)";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto type = index.FindDefinition(source.find("[Button") + 1, PositionEncoding::Utf8);
  const auto field = index.FindDefinition(source.find("label: 'run'"), PositionEncoding::Utf8);
  if (!type || !field) {
    ADD_FAILURE() << "missing attribute type or argument definition";
    return;
  }
  const auto text = SourceText::Create(source);
  ASSERT_TRUE(text);
  EXPECT_EQ(type->range.start,
            text->PositionAt(source.find("class Button") + 6, PositionEncoding::Utf8));
  EXPECT_EQ(field->range.start,
            text->PositionAt(source.find("label: string"), PositionEncoding::Utf8));
  const auto hover = index.FindHover(source.find("run():"), PositionEncoding::Utf8);
  if (!hover) {
    ADD_FAILURE() << "missing annotated method hover";
    return;
  }
  EXPECT_EQ(hover->documentation, "Runs the action.");
  const auto argument = index.FindHover(source.find("label: 'run'"), PositionEncoding::Utf8);
  if (!argument) {
    ADD_FAILURE() << "missing attribute argument hover";
    return;
  }
  EXPECT_TRUE(argument->signature.contains("Button.label"));
  const auto tokens = index.SemanticTokens(PositionEncoding::Utf8);
  const auto position = text->PositionAt(source.find("[Button") + 1, PositionEncoding::Utf8);
  const auto annotation = std::ranges::find(tokens, position, &SemanticToken::position);
  ASSERT_NE(annotation, tokens.end());
  EXPECT_EQ(annotation->kind, SemanticTokenKind::Class);
}

TEST(SemanticAttributeTest, CompletesNamedDataUsingGeneratedConstructorSignatures) {
  const std::string header =
      "class Button extends Reflect { label: string = ''; enabled: bool = true; } "
      "class Tag<T> extends Reflect { value: T; } ";
  const auto cases = std::to_array<std::pair<std::string_view, std::string_view>>({
      {"[Button(la|)] class Counter {}", "label:"},
      {"[Button(label: 'x', |)] class Counter {}", "enabled:"},
      {"[Button(en|] class Counter {}", "enabled:"},
      {"[Tag<int>(va|)] class Counter {}", "value:"},
  });
  for (const auto& [annotation, expected] : cases) {
    auto source = header + std::string(annotation);
    const auto offset = source.find('|');
    source.erase(offset, 1);
    const auto parsed = frontend::ParseSourceSnapshot(source);
    const auto query = PrepareArgumentCompletion(*parsed.source, offset);
    if (!query) {
      ADD_FAILURE() << "missing attribute argument query: " << source;
      continue;
    }
    const frontend::SourceInput input{.path = "main.vela", .text = query->source};
    const SemanticIndex index(input.path, std::span(&input, 1));
    const auto completions = index.CompleteArguments(*query);
    ASSERT_EQ(completions.size(), 1U) << query->source;
    EXPECT_EQ(completions.front().label, expected);
    if (expected == "value:") {
      EXPECT_TRUE(completions.front().detail.contains("int"));
    }
  }
}

TEST(SemanticCompletionTest, LocalShadowingAndBuiltinConstructorsKeepTheirDetails) {
  const std::string source = "func main(): void { const sleep = 42; sleep; ByteArray(1); }";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  const SemanticIndex index(input.path, std::span(&input, 1));
  const auto local = index.Complete(source.rfind("sleep") + std::string_view("sleep").size());
  const auto shadow = std::ranges::find(local, "sleep", &Completion::label);
  ASSERT_NE(shadow, local.end());
  EXPECT_EQ(std::ranges::count(local, "sleep", &Completion::label), 1);
  EXPECT_EQ(shadow->kind, CompletionKind::Constant);
  EXPECT_EQ(shadow->detail, "int");
  const auto constructors =
      index.Complete(source.find("ByteArray") + std::string_view("ByteArray").size());
  const auto bytes = std::ranges::find(constructors, "ByteArray", &Completion::label);
  ASSERT_NE(bytes, constructors.end());
  EXPECT_EQ(std::ranges::count(constructors, "ByteArray", &Completion::label), 1);
  EXPECT_EQ(bytes->kind, CompletionKind::Type);
  EXPECT_NE(bytes->detail.find("constructor(length: int)"), std::string::npos);
}

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

TEST(GenericSemanticTest, RenamePreservesEmbeddedStandardAndSdkSources) {
  for (const std::string_view source : {
           "func main(): void { let value = Math.sqrt(4.0); print(value); }",
           "import 'sdk:test'; func main(): void { let value = 42; print(value); }",
       }) {
    SCOPED_TRACE(source);
    const frontend::SourceInput input{.path = "main.vela", .text = source};
    const SemanticIndex index(input.path, std::span(&input, 1));
    const auto edits = index.RenameSymbol(source.find("value ="), "answer", PositionEncoding::Utf8);
    ASSERT_TRUE(edits) << edits.error();
    ASSERT_EQ(edits->size(), 2U);
    EXPECT_EQ(edits->front().path, edits->back().path);
    EXPECT_FALSE(index.RenameSymbol(source.find("value ="), "print", PositionEncoding::Utf8));
  }
}

}  // namespace
}  // namespace vela::tooling
