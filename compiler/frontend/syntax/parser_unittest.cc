#include "compiler/frontend/syntax/parser.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/lexer.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::frontend {
namespace {

TEST(ParserTest, RejectsDestructuredCollectionBindings) {
  for (const std::string_view binding : {"[key, value]", "[key]", "[key,", "["}) {
    SCOPED_TRACE(binding);
    const auto parsed =
        ParseSource("func f(): void { for (const " + std::string(binding) + " in values) {} }");
    ASSERT_FALSE(parsed.diagnostics.empty());
    EXPECT_EQ(parsed.diagnostics.front().message, "expected name after 'const' in collection loop");
  }
}

TEST(ParserTest, ParsesRadixIntegersWithSignedBoundsAndPreservesSpelling) {
  constexpr auto kLiterals = std::to_array<std::pair<std::string_view, std::int64_t>>({
      {"0x2a", 42},
      {"0X2A", 42},
      {"0x1e2", 482},
      {"0b101010", 42},
      {"0B101010", 42},
      {"0o52", 42},
      {"0O52", 42},
      {"-0x2a", -42},
      {"-0b101010", -42},
      {"-0o52", -42},
      {"0x0000", 0},
      {"-0b0", 0},
      {"0o000", 0},
      {"052", 52},
      {"0x7fffffffffffffff", std::numeric_limits<std::int64_t>::max()},
      {"-0x8000000000000000", std::numeric_limits<std::int64_t>::min()},
      {"0o777777777777777777777", std::numeric_limits<std::int64_t>::max()},
      {"-0o1000000000000000000000", std::numeric_limits<std::int64_t>::min()},
      {
          "0b111111111111111111111111111111111111111111111111111111111111111",
          std::numeric_limits<std::int64_t>::max(),
      },
      {
          "-0b1000000000000000000000000000000000000000000000000000000000000000",
          std::numeric_limits<std::int64_t>::min(),
      },
  });
  for (const auto& [literal, expected] : kLiterals) {
    SCOPED_TRACE(literal);
    const auto parsed = ParseSource("func f(): int { return " + std::string(literal) + "; }");
    ASSERT_TRUE(parsed.diagnostics.empty());
    const auto& expression = *parsed.program.functions.front().body->statements.front()->expression;
    EXPECT_EQ(expression.kind, Expression::Kind::IntegerLiteral);
    EXPECT_EQ(expression.text, literal);
    EXPECT_EQ(expression.IntegerValue(), expected);
  }
}

TEST(LexerTest, DiagnosesMalformedRadixIntegersAndRecoversAtNextToken) {
  for (const std::string_view literal : {
           "0x",
           "0X",
           "0b",
           "0B",
           "0o",
           "0O",
           "0xg",
           "0x12g",
           "0b2",
           "0b102",
           "0o8",
           "0o789",
           "0x_1",
           "0b1_0",
           "0o7_0",
           "0x1p2",
           "0b1e2",
           "0o7foo",
       }) {
    SCOPED_TRACE(literal);
    const auto lexed = Lexer("\n " + std::string(literal) + "; 42").Tokenize();
    ASSERT_EQ(lexed.diagnostics.size(), 1U);
    const auto& diagnostic = lexed.diagnostics.front();
    EXPECT_EQ(diagnostic.location.line, 2U);
    EXPECT_EQ(diagnostic.location.column, 2U);
    EXPECT_EQ(diagnostic.message, literal.size() == 2 ? "integer radix prefix requires digits"
                                                      : "invalid digit in integer literal");
    ASSERT_EQ(lexed.tokens.size(), 3U);
    EXPECT_EQ(lexed.tokens.front().kind, TokenKind::Semicolon);
    EXPECT_EQ(lexed.tokens.at(1).lexeme, "42");
  }
}

TEST(LexerTest, SeparatesRadixIntegersFromOperatorsAndMemberAccess) {
  const auto lexed = Lexer("0xFe+0b10-0o7;0x2a.toString();1.5e+2").Tokenize();
  ASSERT_TRUE(lexed.diagnostics.empty());
  ASSERT_EQ(lexed.tokens.size(), 14U);
  EXPECT_EQ(lexed.tokens.front().lexeme, "0xFe");
  EXPECT_EQ(lexed.tokens.at(2).lexeme, "0b10");
  EXPECT_EQ(lexed.tokens.at(4).lexeme, "0o7");
  EXPECT_EQ(lexed.tokens.at(6).lexeme, "0x2a");
  EXPECT_EQ(lexed.tokens.at(7).kind, TokenKind::Dot);
  EXPECT_EQ(lexed.tokens.at(12).kind, TokenKind::DoubleLiteral);
}

TEST(ParserTest, PreservesBinaryPrecedenceAndLeftAssociativity) {
  constexpr auto kOperators = std::to_array<std::pair<std::string_view, TokenKind>>({
      {"||", TokenKind::OrOr},
      {"&&", TokenKind::AndAnd},
      {"|", TokenKind::Pipe},
      {"^", TokenKind::Caret},
      {"&", TokenKind::Ampersand},
      {"==", TokenKind::EqualEqual},
      {"<", TokenKind::Less},
      {"<<", TokenKind::ShiftLeft},
      {"+", TokenKind::Plus},
      {"*", TokenKind::Star},
  });
  for (std::size_t lower = 0; lower < kOperators.size(); ++lower) {
    for (std::size_t higher = lower; higher < kOperators.size(); ++higher) {
      const auto& [left_text, left_op] = kOperators.at(lower);
      const auto& [right_text, right_op] = kOperators.at(higher);
      const std::string source = "func f(): int { return a " + std::string(left_text) + " b " +
                                 std::string(right_text) + " c; }";
      SCOPED_TRACE(source);
      const auto parsed = ParseSource(source);
      ASSERT_TRUE(parsed.diagnostics.empty());
      const auto& expression =
          *parsed.program.functions.front().body->statements.front()->expression;
      EXPECT_EQ(expression.kind, Expression::Kind::Binary);
      EXPECT_EQ(expression.operation, left_op);
      if (lower == higher) {
        ASSERT_NE(expression.left, nullptr);
        EXPECT_EQ(expression.left->operation, left_op);
      } else {
        ASSERT_NE(expression.right, nullptr);
        EXPECT_EQ(expression.right->operation, right_op);
      }
    }
  }
}

TEST(ParserTest, SplitsTypeClosingsFromAdjacentAssignmentWithoutConsumingOuterCommas) {
  for (const std::string_view source : {
           "type Identity<T>=T;",
           "type Bounded<T extends Box<int>>=T;",
           "type Bounded<T extends Box<Box<int>>>=T;",
           "type Bounded<T extends Box<Box<Box<int>>>>=T;",
           "func use():void{const value:Box<int>=Box<int>();}",
           "func use():void{const value:Box<Box<int>>=Box<Box<int>>();}",
           "func use():void{const value:Box<Box<Box<int>>>=Box<Box<Box<int>>>();}",
       }) {
    SCOPED_TRACE(source);
    const auto parsed = ParseSource(source);
    EXPECT_TRUE(parsed.diagnostics.empty());
  }
  const auto parsed = ParseSource(
      "func use<T extends Map<string,Map<int,int>>,U>(value:Map<string,Map<int,int>>,other:U):U"
      "{return other;}");
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto& function = parsed.program.functions.front();
  ASSERT_EQ(function.type_parameters.size(), 2U);
  EXPECT_EQ(function.type_parameters.back().name, "U");
  ASSERT_EQ(function.parameters.size(), 2U);
  EXPECT_EQ(function.parameters.back().type.name, "U");
  EXPECT_EQ(function.parameters.front().type.arguments.size(), 2U);
  EXPECT_EQ(function.parameters.front().type.arguments.back().arguments.size(), 2U);
}

TEST(ParserTest, PreservesTypeOperatorGrammarAndAssignmentAssociativity) {
  const auto typed = ParseSource("func f(): bool { return (a + b as int) < c << d; }");
  ASSERT_TRUE(typed.diagnostics.empty());
  const auto& comparison = *typed.program.functions.front().body->statements.front()->expression;
  EXPECT_EQ(comparison.operation, TokenKind::Less);
  ASSERT_NE(comparison.left, nullptr);
  EXPECT_EQ(comparison.left->kind, Expression::Kind::TypeCast);
  ASSERT_NE(comparison.left->operand, nullptr);
  EXPECT_EQ(comparison.left->operand->operation, TokenKind::Plus);
  ASSERT_NE(comparison.right, nullptr);
  EXPECT_EQ(comparison.right->operation, TokenKind::ShiftLeft);
  EXPECT_TRUE(ParseSource("func f(): bool { return a == b is int; }").diagnostics.empty());

  const auto assigned = ParseSource("func f(): void { a = b = c; }");
  ASSERT_TRUE(assigned.diagnostics.empty());
  const auto& assignment = *assigned.program.functions.front().body->statements.front()->expression;
  EXPECT_EQ(assignment.operation, TokenKind::Equal);
  ASSERT_NE(assignment.right, nullptr);
  EXPECT_EQ(assignment.right->operation, TokenKind::Equal);

  const auto coalesced = ParseSource("func f(): bool { return a ?? b || c; }");
  ASSERT_TRUE(coalesced.diagnostics.empty());
  const auto& logical = *coalesced.program.functions.front().body->statements.front()->expression;
  EXPECT_EQ(logical.operation, TokenKind::OrOr);
  ASSERT_NE(logical.left, nullptr);
  EXPECT_EQ(logical.left->operation, TokenKind::QuestionQuestion);

  for (const auto* const expression : {"a < b is int", "a is int + b", "a +", "a <", "a *"}) {
    SCOPED_TRACE(expression);
    EXPECT_FALSE(ParseSource("func f(): int { return " + std::string(expression) + "; }")
                     .diagnostics.empty());
  }
}

}  // namespace
}  // namespace vela::frontend
