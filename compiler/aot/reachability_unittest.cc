#include "compiler/aot/reachability.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/declaration_ids.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "runtime/isolate.h"
#include "runtime/program_layouts.h"

namespace vela::compiler::aot {
namespace {

TEST(ReachabilityTest, PrunedOverriddenGenericMethodsLeaveInstantiableLayoutDescriptions) {
  const frontend::SourceInput input{
      .path = "main.vela",
      .text =
          "func identity<T>(value: T): T { return value; } "
          "class Base<T> { value: T; read(): T { return this.value; } } "
          "class Derived<T> extends Base<T> { extra: T; "
          "constructor { super(value: this.extra); } "
          "override read(): T { return identity<T>(this.extra); } } "
          "func main(): int { const value = Derived<int>(extra: 21); "
          "return value.read() + value.value; }",
  };
  auto prepared = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
  ASSERT_TRUE(prepared.Succeeded());
  std::optional<std::uint32_t> main;
  PruneNativeProgram(prepared.program, {}, main);
  runtime::Isolate isolate;
  runtime::LayoutCache cache;
  const auto layouts = runtime::InstantiateLayouts(prepared.program.layouts, isolate, cache);
  EXPECT_FALSE(layouts.classes.empty());
}

TEST(ReachabilityTest, CompactFunctionIndexesDoNotRenumberDeclarationOrVersionIdentities) {
  const frontend::SourceInput input{
      .path = "main.vela",
      .text =
          "func unused(): int { return 0; } "
          "func used(): int { return 42; } export func main(): int { return used(); }",
  };
  auto parsed = frontend::LoadSources(std::span(&input, 1), input.path);
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = frontend::TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.diagnostics.empty());
  frontend::DeclarationIds identities;
  constexpr std::uint64_t kRevision = 7;
  auto prepared = ir::PrepareProgram(parsed.program, checked.model,
                                     {.declarations = &identities, .revision = kRevision});
  ASSERT_TRUE(prepared.Succeeded());
  auto& program = prepared.program;
  const auto old = std::ranges::find(program.functions, "used", &ir::Function::name);
  ASSERT_NE(old, program.functions.end());
  const auto identity = old->identity;
  const auto old_index = old - program.functions.begin();
  const std::array<std::string, 1> exports{"main"};
  std::optional<std::uint32_t> main;
  PruneNativeProgram(program, exports, main);
  const auto current = std::ranges::find(program.functions, "used", &ir::Function::name);
  ASSERT_NE(current, program.functions.end());
  EXPECT_NE(current - program.functions.begin(), old_index);
  EXPECT_EQ(current->identity, identity);
}

TEST(ReachabilityTest, ErasedLocalValuesDoNotRetainUnreferencedClassMethods) {
  for (const auto* const expression : {"42", "Json.parse('42')", "erase(42)", "parsed()"}) {
    SCOPED_TRACE(expression);
    const std::string source =
        "class Unused { value: int = 42; read(): int { return this.value; } } "
        "func erase(value: dynamic): dynamic { return value; } "
        "func parsed(): dynamic { return Json.parse('42'); } "
        "export func main(): int { const erased: dynamic = " +
        std::string(expression) + "; return erased as int; }";
    const frontend::SourceInput input{.path = "main.vela", .text = source};
    auto prepared = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
    ASSERT_TRUE(prepared.Succeeded());
    const std::array<std::string, 1> exports{"main"};
    std::optional<std::uint32_t> main;
    PruneNativeProgram(prepared.program, exports, main);
    EXPECT_FALSE(std::ranges::any_of(prepared.program.functions, [](const ir::Function& function) {
      return function.name.starts_with("Unused.");
    }));
    EXPECT_TRUE(std::ranges::any_of(prepared.program.functions, [](const ir::Function& function) {
      return function.name == "main";
    }));
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
    ASSERT_TRUE(prepared.Succeeded());
    const std::array<std::string, 1> exports{"main"};
    std::optional<std::uint32_t> main;
    PruneNativeProgram(prepared.program, exports, main);
    EXPECT_TRUE(std::ranges::any_of(prepared.program.functions, [](const ir::Function& function) {
      return function.name == "Box.read";
    }));
    EXPECT_TRUE(std::ranges::any_of(prepared.program.functions, [](const ir::Function& function) {
      return function.name == "Box.toJson";
    }));
  }
}

}  // namespace
}  // namespace vela::compiler::aot
