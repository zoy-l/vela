#include "compiler/frontend/syntax/parser.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::frontend {
namespace {

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
