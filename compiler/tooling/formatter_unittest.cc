#include <cstddef>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/token.h"
#include "compiler/tooling/formatter.h"

namespace vela::tooling {
namespace {

TEST(FormatterTest, FormatsMapEntriesAndPreservesLambdaParenthesesIdempotently) {
  const auto formatted = FormatSource(
      "func main():void{const empty:Map<string,int>={};"
      "const make:(int)=>Map<string,int>=x=>({'key':x});"
      "const values={true?'a':'b':{'inner':[1,2]},'last':3,};"
      "print(make(42).get('key'));}");
  ASSERT_TRUE(formatted) << frontend::DiagnosticsText(formatted.error());
  EXPECT_TRUE(formatted->contains("= {};"));
  EXPECT_TRUE(formatted->contains("x => ({'key': x})"));
  EXPECT_TRUE(formatted->contains("true ? 'a' : 'b': {'inner': [1, 2]}"));
  const auto second = FormatSource(*formatted);
  ASSERT_TRUE(second) << frontend::DiagnosticsText(second.error());
  EXPECT_EQ(*second, *formatted);
}

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
      "class Pair<T,U>{a:T;b:U;}func main():int{const a:Pair<int,int> ="
      "Pair<int,int>(a:-1,b:- -2);if(a.a<a.b){return -a.a;}return 0;}");
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->contains("class Pair<T, U>"));
  EXPECT_TRUE(result->contains("const a: Pair<int, int> = Pair<int, int>(a: -1, b: - -2);"));
  EXPECT_TRUE(result->contains("if (a.a < a.b)"));
  EXPECT_TRUE(result->contains("return -a.a;"));
}

TEST(FormatterTest, PreservesNestedBoundsAndGenericMethodDeclarationsAndApplications) {
  const auto result = FormatSource(
      "interface Reader<T>{read<U extends Map<string,Future<T[]>>>(value:U):T;}"
      "class Box<T extends Reader<int>>{"
      "apply<U extends Map<string,Future<T[]>>>(value:U):U{return value;}}"
      "type Values<T extends Reader<int>>=Map<string,Future<T[]>>;"
      "func use<T extends U,U extends Reader<int>>(value:T):int{return value.read<Map<"
      "string,Future<int[]>>>(Map<string,Future<int[]>>());}");
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->contains("read<U extends Map<string, Future<T[]>>>(value: U): T;"));
  EXPECT_TRUE(result->contains("class Box<T extends Reader<int>>"));
  EXPECT_TRUE(result->contains("func use<T extends U, U extends Reader<int>>"));
  EXPECT_TRUE(result->contains("read<Map<string, Future<int[]>>>(Map<string, Future<int[]>>());"));
  const auto repeated = FormatSource(*result);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(*repeated, *result);
}

TEST(FormatterTest, SpacesConditionalOperatorsWithoutChangingNullableTypesOrNamedArguments) {
  const auto result = FormatSource(
      "func choose(value:int?,flag:bool):int?{return flag?(value??0):flag?null:-1;}"
      "func main():int{return choose(value:true?1:2,flag:true)??0;}");
  ASSERT_TRUE(result);
  EXPECT_EQ(*result,
            "func choose(value: int?, flag: bool): int? {\n"
            "  return flag ? (value ?? 0) : flag ? null : -1;\n}\n\n"
            "func main(): int {\n  return choose(value: true ? 1 : 2, flag: true) ?? 0;\n}\n");
  const auto repeated = FormatSource(*result);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(*repeated, *result);
}

TEST(FormatterTest, ConditionalColonDoesNotEndASwitchCaseLabel) {
  const auto result =
      FormatSource("func main():int{switch(1){case true?1:2:return 42;default:return 0;}}");
  ASSERT_TRUE(result);
  EXPECT_EQ(*result,
            "func main(): int {\n  switch (1) {\n    case true ? 1 : 2:\n"
            "      return 42;\n    default:\n      return 0;\n  }\n}\n");
}

TEST(FormatterTest, RetainsCommentPlacementAndEscapeSpellings) {
  constexpr std::string_view kSource =
      "// header\r\nfunc main():void{const text=\"你好\\u0041 ${\"x\" /*inside*/}\"; // tail\r\n"
      "/* next */print(text);}";
  const auto result = FormatSource(kSource);
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->contains("'你好\\u0041 ${'x' /*inside*/}'; // tail\n"));
  EXPECT_TRUE(result->contains("\n  /* next */ print(text);"));
  EXPECT_FALSE(result->contains('\r'));
}

TEST(FormatterTest, FormatsContextualAndInferredLambdasWithoutAddingAnnotations) {
  const auto result = FormatSource(
      "func main():void{const values=[2,1];values.sort((left,right)=>left-right);"
      "values.forEach(value=>{print(value);});const twice=(value:int)=>value*2;}");
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->contains("values.sort((left, right) => left - right);"));
  EXPECT_TRUE(result->contains("values.forEach(value => {"));
  EXPECT_TRUE(result->contains("const twice = (value: int) => value * 2;"));
  const auto second = FormatSource(*result);
  ASSERT_TRUE(second);
  EXPECT_EQ(*result, *second);
}

TEST(FormatterTest, SpacesFunctionTypesNamedArgumentsAndOperators) {
  const auto result = FormatSource(
      "func apply(value:int,operation:(int)=>int):int{return operation(value);}"
      "func make():()=>int{const values:int[]=[1,2];"
      "return ():int=>values[0]+values[1];}");
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_EQ(*result,
            "func apply(value: int, operation: (int) => int): int {\n"
            "  return operation(value);\n}\n\n"
            "func make(): () => int {\n  const values: int[] = [1, 2];\n"
            "  return (): int => values[0] + values[1];\n}\n");
}

TEST(FormatterTest, DefaultsToSingleQuotesWithoutChangingEscapedValues) {
  constexpr std::string_view kSource = R"(func main(): void {
    print("it's \"quoted\" \\ \n \u0041 \${literal}");
    print('already \'single\' and "double"');
    print("${"${'nested'}"}");
  })";
  const auto result = FormatSource(kSource);
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->contains(R"(print('it\'s "quoted" \\ \n \u0041 \${literal}');)"));
  EXPECT_TRUE(result->contains(R"(print('already \'single\' and "double"');)"));
  EXPECT_TRUE(result->contains(R"(print('${'${'nested'}'}');)"));
  const auto repeated = FormatSource(*result);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(*repeated, *result);
}

TEST(FormatterTest, SeparatesTopLevelDeclarationsAndPreservesAnInnerBlankLine) {
  const auto result = FormatSource(
      "import \"one.vela\";import \"two.vela\";"
      "class A{value:int;}func main():void{let value=1;\n\n\nprint(value);}");
  ASSERT_TRUE(result);
  EXPECT_EQ(*result,
            "import 'one.vela';\nimport 'two.vela';\n\nclass A {\n  value: int;\n}\n\n"
            "func main(): void {\n  let value = 1;\n\n  print(value);\n}\n");
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
        "import \"math.vela\" as math; export func root(value:double):double{return "
        "math.sqrt(value);}",
        "class Counter{value:int;private "
        "step:int=1;constructor{this.value=this.value+step;}"
        "next():int{this.value=this.value+1;return this.value;}}",
        "func main():int{const op:(int,int)=>int=(x:int,y:int):int=>{return x+y;};return "
        "op(40,2);}",
        "func main():int{switch(2){case 1:case 2:return 42;default:return 0;}}",
        "class E{message:string;}func main():void{try{throw "
        "E(message:\"x\");}catch(e:E){print(e.message);}"
        "catch{print(\"other\");}finally{print(\"done\");}}",
        "func main():int{const x:Map<int,string> =Map<int,string>();return match(x.size){"
        "42=>42,_=>0,};}",
        "func main():void{const x:int?[][]=[[40,null],[2]];print(\"${(\"${\"nested\"}\")}\");}",
        R"(func main():void{print("apostrophe ' and escaped \"quotes\"");print('${'${"nested"}'}');})",
        R"(func main():void{print('\\\'\\\\\"\u{1F600}\${notInterpolation}');})",
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
