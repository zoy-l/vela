#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker_test_support.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class StringLanguageTest : public testing::Test {
 protected:
  explicit StringLanguageTest(std::size_t gc_threshold = 1) : isolate_(gc_threshold) {}

  vm::ExecutionResult Run(std::string_view source) {
    compiled_ = CompileSource(source, isolate_);
    if (!compiled_.Succeeded()) {
      for (const auto& diagnostic : compiled_.diagnostics) {
        ADD_FAILURE() << diagnostic.location.line << ':' << diagnostic.location.column << ": "
                      << diagnostic.message;
      }
      return {};
    }
    const auto main = compiled_.program.FindFunction("main");
    if (!main.IsValid()) {
      ADD_FAILURE() << "missing main";
      return {};
    }
    const auto index = main.index();
    return vm::VirtualMachine(isolate_).Execute(compiled_.program, index);
  }

  std::string Text(std::string_view source) {
    const auto result = Run(source);
    if (!result.Succeeded() || !result.value.IsString()) {
      ADD_FAILURE() << "string execution failed: " << static_cast<int>(result.status);
      return {};
    }
    return std::string(result.value.AsString()->value());
  }

  runtime::Isolate& isolate() { return isolate_; }
  CompileResult& compilation() { return compiled_; }

 private:
  runtime::Isolate isolate_;
  CompileResult compiled_;
};

class StringAllocationTest : public StringLanguageTest {
 protected:
  StringAllocationTest() : StringLanguageTest(runtime::kDefaultGcThreshold) {}
};

TEST_F(StringLanguageTest, DecodesEscapesUnicodeAndEmbeddedNull) {
  constexpr auto kExpected = std::to_array("\"\\\n\r\t\b\f\v\0A中😀😀");
  EXPECT_EQ(
      Text(
          R"(func main(): string { return "\"\\\n\r\t\b\f\v\0\u0041\u{4e2d}\u{1F600}\uD83D\uDE00"; })"),
      std::string_view(kExpected.data(), kExpected.size() - 1));
}

TEST_F(StringLanguageTest, EscapedInterpolationAndUnicodeDollarRemainLiteral) {
  EXPECT_EQ(Text(R"(func main(): string { return "\${missing}\u0024{alsoMissing} $plain"; })"),
            "${missing}${alsoMissing} $plain");
}

TEST_F(StringLanguageTest, AcceptsSingleQuotesEscapesAndNestedInterpolation) {
  EXPECT_EQ(Text(R"(func main(): string { return 'it\'s "quoted" \u{1F600}'; })"),
            "it's \"quoted\" 😀");
  EXPECT_EQ(Text(R"(func main(): string { return '${'${"nested"}'}'; })"), "nested");
  EXPECT_EQ(Text(R"(func main(): string { return '\${missing}\u0024{alsoMissing}'; })"),
            "${missing}${alsoMissing}");
}

TEST_F(StringLanguageTest, PreservesDecodedQuotesAndEmptyInterpolation) {
  EXPECT_EQ(Text(R"(func main(): string { return "\"quoted\""; })"), "\"quoted\"");
  EXPECT_EQ(Text(R"(func main(): string { return "${""}"; })"), "");
}

TEST_F(StringLanguageTest, HexEscapesEncodeUnicodeScalarsAndApostrophes) {
  EXPECT_EQ(Text(R"(func main(): string { return "\x41\xE9\'"; })"), "Aé'");
  EXPECT_EQ(Text(R"(func main(): string {
    const text = "\u007F\u0080\u07FF\u0800\uD7FF\uE000\uFFFF\u{10000}\u{10FFFF}";
    return "${text.length.toString()}/${text.byteLength.toString()}";
  })"),
            "9/25");
}

TEST_F(StringLanguageTest, NestedInterpolationCommentsAndConstructorExpressions) {
  EXPECT_EQ(Text(R"(
class Item { text: string; }
func main(): string {
  return "[${Item(text: "${"内" + "层"}").text /* } */}]";
})"),
            "[内层]");
}

TEST_F(StringLanguageTest, InterpolationCapturesValuesLeftToRightExactlyOnce) {
  EXPECT_EQ(Text(R"(
class Counter {
  value: int;
  next(): string { this.value = this.value + 1; return this.value.toString(); }
}
func main(): string {
  const counter = Counter(value: 0);
  let text = "before";
  const result = "${text}:${text = "after"}:${counter.next()}:${counter.next()}";
  return "${result}:${counter.value.toString()}";
})"),
            "before:after:1:2:2");
}

TEST_F(StringLanguageTest, ConvertsPrimitivesExplicitlyWithStableFormatting) {
  EXPECT_EQ(Text(R"(
func main(): string {
  return "${(0 - 42).toString()},${1.25.toString()},${(-0.0).toString()},${true.toString()},${false.toString()}";
})"),
            "-42,1.25,-0,true,false");
}

TEST_F(StringLanguageTest, JoinAcceptsEmptySingletonAndNamedArguments) {
  EXPECT_EQ(Text(R"(
func main(): string {
  return "${[].join(",")}|${["中"].join("---")}|${["a", "", "b"].join(separator: "😀")}";
})"),
            "|中|a😀😀b");
}

TEST_F(StringLanguageTest, FormatsPrimitivesOnlyInStringContexts) {
  EXPECT_EQ(Text(R"(
func main(): string {
  const count = 42;
  return '${count}/${1.25}/${-0.0}/${true}/${false}|' + count + '|' +
         (true + '!') + '|' + (1.5 + '!') + '|' + (1 + 2 + '3') + '|' + ('1' + 2 + 3);
})"),
            "42/1.25/-0/true/false|42|true!|1.5!|33|123");
}

TEST_F(StringLanguageTest, PrimitiveArrayJoinFormatsWithoutElementConversions) {
  EXPECT_EQ(Text(R"(
func main(): string {
  const integers = [0 - 42, 0, 42];
  const doubles = [1.25, -0.0, 2.0];
  const booleans = [true, false];
  const empty: int[] = [];
  return '${integers.join(",")}|${doubles.join("/")}|${booleans.join(separator: ":")}|${empty.join("-")}';
})"),
            "-42,0,42|1.25/0/2|true:false|");
}

TEST_F(StringLanguageTest, StringFormattingSnapshotsMutableValuesBeforeFollowingEffects) {
  EXPECT_EQ(Text(R"(
class Counter {
  value: int;
  next(): int { this.value = this.value + 1; return this.value; }
}
func main(): string {
  const counter = Counter(value: 0);
  let value = 1;
  const text = '${value}:${value = 2}:${counter.next()}:${counter.next()}';
  const concat = value + (value = 3).toString();
  return '${text}|${concat}|${counter.value}';
})"),
            "1:2:1:2|23|2");
}

TEST_F(StringLanguageTest, FormatsNullAndNullablePrimitivesWithoutChangingTheirTypes) {
  EXPECT_EQ(Text(R"(
func main(): string {
  let value: int? = null;
  const words: string?[] = ['a', null, 'b'];
  const numbers: int?[] = [1, null, 2];
  const flags: bool?[] = [true, null, false];
  const doubles: double?[] = [1.5, null, -0.0];
  return '${value}|${null}|' + value + '|' +
         words.join(',') + '|' + numbers.join(',') + '|' + flags.join(',') + '|' + doubles.join(',');
})"),
            "null|null|null|a,,b|1,,2|true,,false|1.5,,0");
}

TEST_F(StringAllocationTest, PrimitiveFormattingAllocatesOnlyTheFinalString) {
  auto& isolate = this->isolate();
  auto& compiled = compilation();
  struct Case final {
    std::string_view source;
    std::size_t allocations;
  };
  const auto cases = std::to_array<Case>({
      {.source = R"(func main(): string { return '${42}:${1.25}:${true}'; })", .allocations = 1},
      {.source = R"(func main(): string { return 'value=' + 42; })", .allocations = 1},
      {.source = R"(func main(): string { return [42, 43].join(','); })", .allocations = 2},
  });
  for (const auto& test : cases) {
    static_cast<void>(Text(test.source));
    auto& heap = isolate.heap();
    heap.Collect();
    const auto before = heap.object_count();
    const auto result = vm::VirtualMachine(isolate).Execute(compiled.program, 0);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(heap.object_count(), before + test.allocations);
  }
}

TEST_F(StringLanguageTest, JoinIsAnOrdinaryUserDeclarationName) {
  EXPECT_EQ(Text(R"(func join(): string { return 'user'; }
                    func main(): string { return join() + ['a', 'b'].join('-'); })"),
            "usera-b");
  EXPECT_TRUE(frontend::CheckTestSource("class join {} func main(): void { const x = join(); }")
                  .Succeeded());
  EXPECT_TRUE(
      frontend::CheckTestSource("enum join { value } func main(): void { const x = join.value; }")
          .Succeeded());
}

TEST_F(StringLanguageTest, JoinEvaluatesReceiverThenSeparatorExactlyOnce) {
  EXPECT_EQ(Text(R"(
class Source {
  state: string;
  separator(): string { this.state = this.state + "s"; return ":"; }
  parts(): string[] { this.state = this.state + "p"; return [this.state]; }
}
func main(): string {
  const source = Source(state: "");
  const text = source.parts().join(source.separator());
  return "${text}|${source.state}";
})"),
            "p|ps");
}

TEST_F(StringLanguageTest, BuilderSnapshotsStayImmutableAndCanBeAppendedAgain) {
  EXPECT_EQ(Text(R"(
func main(): string {
  const builder = String();
  const empty = builder.toString();
  builder.append("");
  builder.append(text: "中");
  const first = builder.toString();
  builder.append(first);
  builder.append("😀");
  return "${empty}|${first}|${builder.toString()}";
})"),
            "|中|中中😀");
}

TEST_F(StringLanguageTest, BuilderSurvivesFieldsArraysAliasesAndCollection) {
  auto& isolate = this->isolate();
  auto& compiled = compilation();
  const auto result = Run(R"(
class Store { builder: String; builders: String[][]; }
func grow(builder: String): void {
  for (let i = 0; i < 1000; i = i + 1) { builder.append("${i.toString()}中"); }
}
func main(): Store {
  const builder = String();
  grow(builder);
  return Store(builder: builder, builders: [[builder]]);
})");
  ASSERT_TRUE(result.Succeeded());
  auto value = result.value;
  runtime::RootRange root(isolate.heap(), &value, 1);
  compiled = CompileResult();
  isolate.heap().Collect();
  const auto builder = *runtime::Heap::GetField(*value.AsObject(), 0);
  auto* snapshot = isolate.heap().BuildString(*builder.AsObject());
  ASSERT_NE(snapshot, nullptr);
  EXPECT_TRUE(snapshot->value().starts_with("0中1中"));
  EXPECT_TRUE(snapshot->value().ends_with("999中"));
  value = runtime::Value::Null();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
  EXPECT_EQ(isolate.heap().allocated_bytes(), 0U);
}

TEST_F(StringLanguageTest, UnicodeApisUseCodePointsAndPreserveUtf8Boundaries) {
  EXPECT_EQ(Text(R"(
func main(): string {
  const text = "A中😀e\u0301";
  return "${text.length.toString()}/${text.byteLength.toString()}/${text[2]}/${text.slice(1, 3)}/${text.indexOf("e").toString()}/${text.includes("中😀").toString()}/${text.startsWith("A中").toString()}/${text.endsWith("\u0301").toString()}";
})"),
            "5/11/😀/中😀/3/true/true/true");
}

TEST_F(StringLanguageTest, EmptyAndMissingSearchAndSliceEdges) {
  EXPECT_EQ(Text(R"(
func main(): string {
  const empty = "";
  const text = "中😀";
  return "${empty.length.toString()},${empty.slice(0, 0)},${text.slice(2, 2)},${text.indexOf("").toString()},${text.indexOf("x").toString()},${text.includes("").toString()},${empty.startsWith("").toString()},${empty.endsWith("").toString()}";
})"),
            "0,,,0,-1,true,true,true");
}

TEST_F(StringLanguageTest, CountsEveryConstructionPathAndTraversesBothDirectionsWithDynamicAccess) {
  const auto result = Run(R"(
func main(): int {
  const text = 'A中😀e\u0301\0';
  const joined = [text, text].join('界');
  const absent: string? = null;
  const formatted = '${text}/${42}/${true}/${absent}';
  const numbers = [12, 34].join('中');
  const builder = String(); builder.append(text); builder.append('ß');
  const snapshot = builder.toString();
  const forward = String(); const backward = String();
  const dynamicText: dynamic = text;
  for (let i = 0; i < dynamicText.length; i++) { forward.append(dynamicText[i]); }
  for (let i = text.length - 1; i >= 0; i--) { backward.append(text[i]); }
  return text.length == 6 && joined.length == 13 && formatted.length == 19 &&
         numbers.length == 5 && snapshot.length == 7 && snapshot.toUpperCase().length == 8 &&
         'İßﬃ'.toLowerCase().length == 4 && 'ßﬃ'.toUpperCase().length == 5 &&
         forward.toString() == text && backward.toString() == '\0\u0301e😀中A' &&
         joined.slice(6, 9) == '界A中' ? 42 : 0;
}
)");
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST_F(StringAllocationTest, InterpolationAllocatesOnlyTheFinalString) {
  auto& isolate = this->isolate();
  auto& compiled = compilation();
  EXPECT_EQ(Text(R"(func main(): string { return "a${"b"}c${"d"}e${"f"}g"; })"), "abcdefg");
  auto& heap = isolate.heap();
  heap.Collect();
  const auto before = heap.object_count();
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program, 0);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(heap.object_count(), before + 1);
  EXPECT_EQ(result.value.AsString()->value(), "abcdefg");
}

TEST_F(StringAllocationTest, JoinAllocatesOnlyTheInputArrayAndFinalString) {
  auto& isolate = this->isolate();
  auto& compiled = compilation();
  EXPECT_EQ(Text(R"(func main(): string { return ["a", "b", "c", "d"].join("-"); })"), "a-b-c-d");
  auto& heap = isolate.heap();
  heap.Collect();
  const auto before = heap.object_count();
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program, 0);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(heap.object_count(), before + 2);
}

TEST_F(StringLanguageTest, SearchHandlesLongRepeatedPrefixesInLinearTime) {
  const std::string prefix(128, 'a');
  const std::string text(4096, 'a');
  const auto source = "func main(): string { const text = \"" + text +
                      R"(b"; return "${text.indexOf(")" + prefix +
                      "b\").toString()},${text.includes(\"" + prefix + "c\").toString()}\"; }";
  EXPECT_EQ(Text(source), "3968,false");
}

class StringBoundsTest : public StringLanguageTest,
                         public testing::WithParamInterface<const char*> {};

TEST_P(StringBoundsTest, ReturnsExplicitRuntimeBoundsError) {
  const auto result = Run(std::string("func main(): string { return ") + GetParam() + "; }");
  EXPECT_EQ(result.status, vm::ExecutionStatus::IndexOutOfBounds);
}

INSTANTIATE_TEST_SUITE_P(InvalidRanges,
                         StringBoundsTest,
                         testing::Values("\"\"[0]",
                                         "\"中\"[-1]",
                                         "\"中\"[1]",
                                         "\"中\".slice(-1, 0)",
                                         "\"中\".slice(1, 0)",
                                         "\"中\".slice(0, 2)",
                                         "\"中\".slice(2, 2)"));

class StringDiagnosticTest : public testing::TestWithParam<const char*> {};

TEST_P(StringDiagnosticTest, RejectsInvalidSyntaxAndImplicitConversions) {
  const auto checked = frontend::CheckTestSource(GetParam());
  EXPECT_FALSE(checked.Succeeded());
  EXPECT_FALSE(checked.diagnostics.empty());
}

INSTANTIATE_TEST_SUITE_P(
    StrictStrings,
    StringDiagnosticTest,
    testing::Values(R"(func main(): string { return "\q"; })",
                    R"(func main(): string { return '\q'; })",
                    R"(func main(): string { return 'unterminated; })",
                    R"(func main(): string { return '${'nested'; })",
                    R"(func main(): string { return "\x1"; })",
                    R"(func main(): string { return "\xGG"; })",
                    R"(func main(): string { return "\u12"; })",
                    R"(func main(): string { return "\u{}"; })",
                    R"(func main(): string { return "\u{110000}"; })",
                    R"(func main(): string { return "\u{1234567}"; })",
                    R"(func main(): string { return "\u{D800}"; })",
                    R"(func main(): string { return "\uD800"; })",
                    R"(func main(): string { return "\uDC00"; })",
                    R"(func main(): string { return "\uD800\u0041"; })",
                    R"(func main(): string { return "${}"; })",
                    R"(func main(): string { return "${"x"; })",
                    R"(func main(): string { return join(["x"], ","); })",
                    R"(func main(): string { return join(parts: ["x"], separator: ","); })",
                    R"(func main(): string { return ["x"].join(1); })",
                    R"(func main(): string { let x: string[]? = null; return x.join(","); })",
                    R"(func main(): string { return [["x"]].join(","); })",
                    R"(class Item {} func main(): string { return '${Item()}'; })",
                    R"(func main(): string { return '${[1, 2]}'; })",
                    R"(class Item {} func main(): string { return Item() + 'x'; })",
                    R"(func main(): void { const x: string = 42; })",
                    R"(func accept(x: string): void {} func main(): void { accept(42); })",
                    R"(func main(): double { return 1 + 1.5; })",
                    R"(func main(): void { if (1) {} })",
                    R"(func main(): string { return [1, true].join(','); })",
                    R"(func main(): string { return "x".slice(0.0, 1); })",
                    R"(func main(): string { return "x"[false]; })",
                    R"(func main(): void { let text = "x"; text[0] = "y"; })",
                    R"(func main(): void { let text = "x"; text.length = 0; })",
                    R"(func main(): void { String().append(1); })",
                    R"(func main(): void { StringBuilder(); })",
                    R"(func main(): void { const value: string = String(); })",
                    R"(func main(): void { const value: String = "immutable"; })",
                    R"(func main(): string { return String().append("x"); })",
                    R"(func main(): bool { const b = String(); return b == b; })",
                    R"(func main(): string { return true.toString(1); })",
                    R"(import "a\0b"; func main(): void {})",
                    R"(import "${"path"}"; func main(): void {})"));

TEST(StringDiagnosticTest, RejectsMalformedUtf8InLiteral) {
  const std::array<std::string_view, 6> invalid = {
      "\x80",
      "\xC0\xAF",
      "\xE2\x82",
      "\xED\xA0\x80",
      "\xF4\x90\x80\x80",
      "\xE2"
      "A\xAC",
  };
  for (const auto bytes : invalid) {
    const auto checked = frontend::CheckTestSource(std::string("func main(): string { return \"") +
                                                   std::string(bytes) + "\"; }");
    EXPECT_FALSE(checked.Succeeded());
  }
}

}  // namespace
}  // namespace vela::compiler
