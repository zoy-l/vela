#include <string_view>

#include "gtest/gtest.h"

#include "compiler/frontend/checker.h"

namespace vela::frontend {
namespace {

class DeclarationErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(DeclarationErrorTest, RejectsInvalidDeclarationsAndConstWrites) {
  EXPECT_FALSE(CheckSource(GetParam()).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    SyntaxAndBindings,
    DeclarationErrorTest,
    testing::Values("int main() { return 1; }",
                    "function main(): int { return 1; }",
                    "func main(int value): int { return value; }",
                    "func main() { return 1; }",
                    "class A { int value; }",
                    "class A { func read(): int { return 1; } }",
                    "func main(): int { int value = 1; return value; }",
                    "func main(): int { let value; return 1; }",
                    "func main(): int { const value: int; return 1; }",
                    "func main(): int { let value = []; return 1; }",
                    "func nothing(): void {} func main(): int { const x = nothing(); return 1; }",
                    "func main(): int { const value = 1; value = 2; return value; }",
                    "func main(): int { let value = 1; value = 2.0; return value; }",
                    "func main(): int { const value: double = 1; return 1; }",
                    "func main(): int { const values = [1]; values = [2]; return 1; }",
                    "class A { value: int; reset(): void { this = A(value: 1); } }",
                    "func main(): int { for (const i = 0; i < 2; i = i + 1) {} return 1; }"));

TEST(DeclarationTest, AcceptsInferenceContextAndMutableReferences) {
  constexpr std::string_view source = R"(
enum Shade { light, dark }
class Counter {
  value: int;
  add(step: int): int { this.value = this.value + step; return this.value; }
}
func read(value: int): int { return value; }
func main(): int {
  const counter = Counter(value: 1);
  const values = [1, 2];
  const empty: int[] = [];
  const shade: Shade = .light;
  const scale = 1.5;
  let count = 0;
  count = counter.add(read(values[0]));
  values[1] = count;
  { let counter = 2; counter = 3; }
  return values[1];
}
)";
  EXPECT_TRUE(CheckSource(source).Succeeded());
}

}  // namespace
}  // namespace vela::frontend
