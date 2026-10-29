#include "compiler/aot/reachability.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/source_loader.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"

namespace vela::compiler::aot {
namespace {

TEST(ReachabilityTest, ErasedLocalValuesDoNotRetainUnreferencedClassMethods) {
  for (const auto expression : {"42", "Json.parse('42')", "erase(42)", "parsed()"}) {
    SCOPED_TRACE(expression);
    const std::string source =
        "class Unused { value: int = 42; read(): int { return this.value; } } "
        "func erase(value: dynamic): dynamic { return value; } "
        "func parsed(): dynamic { return Json.parse('42'); } "
        "export func main(): int { const erased: dynamic = " +
        std::string(expression) + "; return erased as int; }";
    const frontend::SourceInput input{.path = "main.vela", .text = source};
    auto prepared = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
    ASSERT_TRUE(prepared.has_value());
    const std::array<std::string, 1> exports{"main"};
    std::optional<std::uint32_t> main;
    PruneNativeProgram(*prepared, exports, main);
    EXPECT_FALSE(std::ranges::any_of(prepared->functions, [](const ir::Function& function) {
      return function.name.starts_with("Unused.");
    }));
    EXPECT_TRUE(std::ranges::any_of(
        prepared->functions, [](const ir::Function& function) { return function.name == "main"; }));
  }
}

TEST(ReachabilityTest, HostDynamicContractsAndDynamicDispatchRetainTheirMemberClosure) {
  const auto bodies = std::to_array<std::string_view>({
      "export func main(value: dynamic): bool { return value == null; }",
      "export func main(value: dynamic[]): int { return value.length; }",
      "export func main(value: Map<string, dynamic>): int { return value.size; }",
      "export func main(value: Holder): bool { return true; }",
      "export func main(value: () => dynamic): bool { return true; }",
      "export func main(): Future<dynamic> { return Future.value<dynamic>(42); }",
      "export func main(): int { const value: dynamic = Box(); return value.read() as int; }",
      "export func main(): string { return Json.stringify(Box()); }",
  });
  for (const auto body : bodies) {
    SCOPED_TRACE(body);
    const std::string source =
        "class Box { value: int = 42; read(): int { return this.value; } "
        "toJson(): int { return this.value; } } class Holder { value: dynamic; } " +
        std::string(body);
    const frontend::SourceInput input{.path = "main.vela", .text = source};
    auto prepared = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
    ASSERT_TRUE(prepared.has_value());
    const std::array<std::string, 1> exports{"main"};
    std::optional<std::uint32_t> main;
    PruneNativeProgram(*prepared, exports, main);
    EXPECT_TRUE(std::ranges::any_of(prepared->functions, [](const ir::Function& function) {
      return function.name == "Box.read";
    }));
    EXPECT_TRUE(std::ranges::any_of(prepared->functions, [](const ir::Function& function) {
      return function.name == "Box.toJson";
    }));
  }
}

}  // namespace
}  // namespace vela::compiler::aot
