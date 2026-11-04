#include <array>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/lexer.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/typing/checker.h"
#include "compiler/test_support/checker_test_support.h"

namespace vela::frontend {
namespace {

TEST(NativeClassTest, PreservesNativeIdentityAndBoundMemberKeys) {
  auto lexed = Lexer(R"(
export external class Counter = 'app.Counter' {
  constructor(initial: int);
  add(delta: int): int;
  static Create(): Counter?;
  get value(): int;
  set value(amount: int): void;
}
)")
                   .Tokenize();
  ASSERT_TRUE(lexed.diagnostics.empty());
  const auto parsed = Parser(lexed.tokens).Parse();
  ASSERT_TRUE(parsed.diagnostics.empty()) << DiagnosticsText(parsed.diagnostics);
  ASSERT_EQ(parsed.program.classes.size(), 1U);
  const auto& type = parsed.program.classes.front();
  EXPECT_EQ(type.native_key, "app.Counter");
  EXPECT_EQ(type.export_kind, ExportKind::Exported);
  ASSERT_NE(type.constructor, nullptr);
  EXPECT_EQ(type.constructor->native_symbol, "app.Counter::constructor");
  ASSERT_EQ(type.methods.size(), 4U);
  EXPECT_EQ(type.methods[0].native_symbol, "app.Counter::method:add");
  EXPECT_EQ(type.methods[1].native_symbol, "app.Counter::static:Create");
  EXPECT_EQ(type.methods[2].native_symbol, "app.Counter::get:value");
  EXPECT_EQ(type.methods[3].native_symbol, "app.Counter::set:value");
  EXPECT_EQ(type.constructor_kind, ConstructorKind::Explicit);
}

TEST(NativeClassTest, ChecksImportedConstructorsPropertiesBoundMethodsAndNominalTypes) {
  constexpr auto kSources = std::to_array<SourceInput>({
      {
          .path = "native.vela",
          .text = R"(
export external class Counter = 'app.Counter' {
  constructor(initial: int);
  add(delta: int): int;
  static Create(): Counter?;
  get value(): int;
  set value(amount: int): void;
})",
      },
      {
          .path = "main.vela",
          .text = R"(
import 'native';
type Counters = Counter[];
func main(): int {
  const counter = Counter(initial: 1);
  const add: (int) => int = counter.add;
  const values: Counters = [counter];
  const optional: Counter? = Counter.Create();
  counter.value = add(2);
  const dynamicValue: dynamic = values[0];
  if (dynamicValue is Counter) {
    return (dynamicValue as Counter).value;
  }
  return optional == null ? 0 : optional.value;
})",
      },
  });
  const auto checked = CheckTestModules(kSources, "main.vela");
  EXPECT_TRUE(checked.Succeeded()) << DiagnosticsText(checked.diagnostics);
}

TEST(NativeClassTest, RejectsMissingConstructorsReadonlyWritesInheritanceAndJsonDecoding) {
  constexpr auto kPrograms = std::to_array<std::string_view>({
      "external class C = 'app.C' {} func main(): C { return C(); }",
      "external class C = 'app.C' { get value(): int; } "
      "func f(c: C): void { c.value = 1; }",
      "external class C = 'app.C' {} class Derived extends C {}",
      "class Derived extends C {} external class C = 'app.C' {}",
      "external class C = 'app.C' { constructor(); } "
      "func main(): C { return JSON.parse<C>('{}'); }",
      "external class C = 'app.C' {} external class Other = 'app.C' {}",
      "external class C = 'app.C' { get value(): int; set value(v: bool): void; }",
      "external class C = 'app.C' { constructor(x: int); } "
      "func main(): C { return C(unknown: 1); }",
  });
  for (const auto source : kPrograms) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(CheckTestSource(source).Succeeded());
  }
}

TEST(NativeClassTest, RejectsUnsupportedExternalClassSyntax) {
  constexpr auto kDeclarations = std::to_array<std::string_view>({
      "external class C = '' {}",
      "external class C = 'app.class' {}",
      "external class C = 'app.C' { field: int; }",
      "external class C = 'app.C' { constructor() {} }",
      "external class C = 'app.C' { f(): int { return 1; } }",
      "external class C = 'app.C' { private f(): int; }",
      "external class C = 'app.C' { override f(): int; }",
      "external class C = 'app.C' { static get value(): int; }",
      "external class C = 'app.C' { f<T>(value: T): T; }",
      "external class C = 'app.C' { constructor(); constructor(x: int); }",
      "external class C = 'app.C' { async f(): Future<int>; }",
      "external class C = 'app.C' extends Other {}",
      "external class C = 'app.C' implements I {}",
      "external class C = 'app.C'<T> {}",
  });
  for (const auto source : kDeclarations) {
    SCOPED_TRACE(source);
    auto lexed = Lexer(source).Tokenize();
    EXPECT_FALSE(Parser(lexed.tokens).Parse().diagnostics.empty());
  }
}

}  // namespace
}  // namespace vela::frontend
