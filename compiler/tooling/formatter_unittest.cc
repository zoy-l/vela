#include <cstddef>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/frontend/lexer.h"
#include "compiler/frontend/token.h"
#include "compiler/tooling/formatter.h"

namespace vela::tooling {
namespace {

TEST(FormatterTest, FormatsBlocksDeclarationsAndControlFlow) {
  const auto result = FormatSource(
      "func main():int{let value=40;for(let i=0;i<2;i=i+1){value=value+1;}return value;}");
  ASSERT_TRUE(result);
  EXPECT_EQ(*result,
            "func main(): int {\n  let value = 40;\n  for (let i = 0; i < 2; i = i + 1) {\n"
            "    value = value + 1;\n  }\n  return value;\n}\n");
}

TEST(FormatterTest, UsesParserRolesForGenericsAndPrefixOperators) {
  const auto result = FormatSource(
      "class Pair<T,U>{param a:T;param b:U;}func main():int{const a:Pair<int,int> ="
      "Pair<int,int>(a:-1,b:- -2);if(a.a<a.b){return -a.a;}return 0;}");
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->contains("class Pair<T, U>"));
  EXPECT_TRUE(result->contains("const a: Pair<int, int> = Pair<int, int>(a: -1, b: - -2);"));
  EXPECT_TRUE(result->contains("if (a.a < a.b)"));
  EXPECT_TRUE(result->contains("return -a.a;"));
}

TEST(FormatterTest, RetainsCommentPlacementAndLiteralSpellings) {
  constexpr std::string_view kSource =
      "// header\r\nfunc main():void{const text=\"你好\\u0041 ${\"x\" /*inside*/}\"; // tail\r\n"
      "/* next */print(text);}";
  const auto result = FormatSource(kSource);
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->contains("\"你好\\u0041 ${\"x\" /*inside*/}\"; // tail\n"));
  EXPECT_TRUE(result->contains("\n  /* next */ print(text);"));
  EXPECT_FALSE(result->contains('\r'));
}

class FormattingInvariantTest : public testing::TestWithParam<std::string_view> {};

TEST_P(FormattingInvariantTest, IsIdempotent) {
  const auto formatted = FormatSource(GetParam());
  ASSERT_TRUE(formatted) << formatted.error().front().message;
  const auto repeated = FormatSource(*formatted);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(*repeated, *formatted);
}

void ExpectSameTokens(const frontend::LexResult& before, const frontend::LexResult& after) {
  ASSERT_TRUE(after.diagnostics.empty());
  ASSERT_EQ(before.tokens.size(), after.tokens.size());
  for (std::size_t index = 0; index < before.tokens.size(); ++index) {
    const auto& left = before.tokens.at(index);
    const auto& right = after.tokens.at(index);
    EXPECT_EQ(left.kind, right.kind);
    EXPECT_EQ(left.lexeme, right.lexeme);
  }
}

TEST_P(FormattingInvariantTest, PreservesEveryLanguageToken) {
  const auto formatted = FormatSource(GetParam());
  ASSERT_TRUE(formatted);
  ExpectSameTokens(frontend::Lexer(GetParam()).Tokenize(), frontend::Lexer(*formatted).Tokenize());
}

INSTANTIATE_TEST_SUITE_P(
    ProductionSyntax,
    FormattingInvariantTest,
    testing::Values(
        "",
        "// comment only",
        "/* block only */",
        "enum Mode{light,dark,}",
        "import \"std:math\" as math; export func root(value:double):double{return "
        "math.sqrt(value);}",
        "class Counter{public param value:int;private "
        "step:int=1;constructor{this.value=this.value+step;}"
        "next():int{this.value=this.value+1;return this.value;}}",
        "func main():int{const op:(int,int)=>int=(x:int,y:int):int=>{return x+y;};return "
        "op(40,2);}",
        "func main():int{switch(2){case 1:case 2:return 42;default:return 0;}}",
        "class E{param message:string;}func main():void{try{throw "
        "E(message:\"x\");}catch(e:E){print(e.message);}"
        "catch{print(\"other\");}finally{print(\"done\");}}",
        "func main():int{const x:Expected<int,string> =ok(42);return match(x.value()){"
        "42=>42,_=>0,};}",
        "func main():void{const x:int?[][]=[[40,null],[2]];print(\"${(\"${\"nested\"}\")}\");}",
        "func sum(a:int,b:int,c:int,d:int,e:int,f:int,g:int,h:int,i:int,j:int):int{return "
        "a+b+c+d+e+f+g+h+i+j;}"
        "func main():int{return sum(1,2,3,4,5,6,7,8,9,10);}"));

TEST(FormatterTest, RejectsMalformedInputWithoutProducingAReplacement) {
  EXPECT_FALSE(FormatSource("func main():void{const value=;}"));
  EXPECT_FALSE(FormatSource("func main():void{print(\"bad\\q\");}"));
  EXPECT_FALSE(FormatSource("func main():void{ /* missing close"));
}

}  // namespace
}  // namespace vela::tooling
