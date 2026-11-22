#include "compiler/aot/reachability.h"
#include "compiler/aot/native_call_optimizer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/callable.h"
#include "base/layout_description.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"

#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "compiler/native/native_image_data.h"
#include "compiler/test_support/preparation_test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/isolate/program_layouts.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::aot {
namespace {

void PruneMain(ir::Program& program) {
  for (auto& function : program.functions) {
    function.is_exported = function.name == "main";
  }
  std::optional<std::uint32_t> main;
  PruneNativeProgram(program, main);
}

TEST(ReachabilityTest, VirtualSelectorsRetainOverridesAndDevirtualizeOnlyProvenTargets) {
  auto prepared = ir::test::Prepare(R"(
class Base {
  read(): int { return 1; }
  unused(): int { return 0; }
}
class Child extends Base { override read(): int { return 42; } }
class Solo { read(): int { return 42; } }
func polymorphic(value: Base): int { return value.read(); }
func exact(): int { const value: Base = Child(); return value.read(); }
func unique(value: Solo): int { return value.read(); }
export func main(): int { return polymorphic(Base()) + exact() + unique(Solo()); }
)");
  ASSERT_TRUE(prepared.Succeeded());
  auto& program = prepared.program;
  std::optional<std::uint32_t> main;
  PruneNativeProgram(program, main);
  const auto image = native::DescribeNativeImage(program);
  const auto classes = image->image()->classes.values();
  for (std::size_t index = 0; index < classes.size(); ++index) {
    const auto& description = classes[index];
    const auto& targets = program.classes[index].dispatch_targets;
    for (std::size_t selector = 0; selector < targets.size(); ++selector) {
      EXPECT_EQ(description.VirtualTarget(static_cast<std::uint32_t>(selector)), targets[selector]);
    }
    EXPECT_EQ(description.VirtualTarget(runtime::kNoNativeIndex), runtime::kNoNativeIndex);
  }
  EXPECT_FALSE(std::ranges::any_of(
      program.functions, [](const auto& function) { return function.name == "Base.unused"; }));
  DevirtualizeNativeCalls(program);
  const auto polymorphic = std::ranges::find(program.functions, "polymorphic", &ir::Function::name);
  ASSERT_NE(polymorphic, program.functions.end());
  EXPECT_TRUE(std::ranges::any_of(polymorphic->instructions, [](const auto& instruction) {
    return instruction.opcode == Opcode::CallVirtual;
  }));
  for (const auto& [caller, target] :
       {std::pair{"exact", "Child.read"}, std::pair{"unique", "Solo.read"}}) {
    const auto function = std::ranges::find(program.functions, caller, &ir::Function::name);
    ASSERT_NE(function, program.functions.end());
    EXPECT_TRUE(std::ranges::any_of(function->instructions, [&](const auto& instruction) {
      return instruction.opcode == Opcode::Call &&
             program.functions.at(instruction.index).name == target;
    }));
  }
}

TEST(ReachabilityTest, InternalClassesDoNotRetainUnreachableDynamicMemberTables) {
  auto prepared = ir::test::Prepare(R"(
class Counter {
  stored: int = 42;
  get value(): int { return this.stored; }
  set value(next: int): void { this.stored = next; }
  unused(): int { return 0; }
}
export func main(): int { return Counter().value; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  auto& program = prepared.program;
  PruneMain(program);
  EXPECT_FALSE(std::ranges::any_of(program.members, [&](const auto& member) {
    return program.runtime_types.at(member.receiver_type).name == "Counter";
  }));
  EXPECT_FALSE(std::ranges::any_of(program.functions, [](const auto& function) {
    return function.name == "Counter.$set.value" || function.name == "Counter.unused";
  }));
}

TEST(ReachabilityTest, ReflectionRetainsSelectedPublicCodeAndRemapsTargets) {
  const frontend::SourceInput source{
      .path = "main.vela",
      .text = R"(
func unused(): int { return 0; }
class Counter {
  value: int = 1;
  add(amount: int): int { return this.value + amount; }
  private secret(): int { return 7; }
}
export func main(): int { return 1; }
)",
  };
  const frontend::ReflectionSelection selection{
      .module = "main.vela",
      .type = *frontend::ParseTypeSource("Counter"),
      .members =
          {
              {
                  .kind = frontend::ReflectionMemberKind::Method,
                  .name = "add",
                  .type_arguments = {},
                  .location = {},
              },
              {
                  .kind = frontend::ReflectionMemberKind::Method,
                  .name = "secret",
                  .type_arguments = {},
                  .location = {},
              },
              {
                  .kind = frontend::ReflectionMemberKind::Constructor,
                  .name = {},
                  .type_arguments = {},
                  .location = {},
              },
          },
      .source_path = "<reflection>",
      .location = {},
      .source = frontend::ParseSourceSnapshot({}).source,
  };
  auto prepared = ir::PrepareParsed(
      frontend::LoadSources(std::span(&source, 1), source.path, std::span(&selection, 1)));
  ASSERT_TRUE(prepared.Succeeded());
  PruneMain(prepared.program);
  const auto& program = prepared.program;
  EXPECT_FALSE(std::ranges::any_of(program.functions, [](const auto& function) {
    return function.name == "unused" || function.name == "Counter.secret";
  }));
  const auto add = std::ranges::find(program.reflection_members, "add", &reflection::Member::name);
  ASSERT_NE(add, program.reflection_members.end());
  ASSERT_NE(add->target, kInvalidFunctionTarget);
  EXPECT_NE(add->flags & reflection::kVirtual, 0U);
  const auto owner = program.layouts.types.at(add->declaring_type).index;
  const auto target = program.classes.at(owner).dispatch_targets.at(add->target);
  EXPECT_EQ(program.functions.at(target).name, "Counter.add");
  const auto secret =
      std::ranges::find(program.reflection_members, "secret", &reflection::Member::name);
  ASSERT_NE(secret, program.reflection_members.end());
  EXPECT_NE(secret->flags & reflection::kRegistered, 0U);
  EXPECT_EQ(secret->target, kInvalidFunctionTarget);
  const auto constructor =
      std::ranges::find(program.reflection_members, "constructor", &reflection::Member::name);
  ASSERT_NE(constructor, program.reflection_members.end());
  ASSERT_NE(constructor->target, kInvalidFunctionTarget);
  EXPECT_EQ(program.functions.at(constructor->target).name, "$construct.Counter");
}

TEST(ReachabilityTest, PrunedOverriddenGenericMethodsLeaveInstantiableLayoutDescriptions) {
  auto prepared = ir::test::Prepare(
      "func identity<T>(value: T): T { return value; } "
      "class Base<T> { value: T; read(): T { return this.value; } } "
      "class Derived<T> extends Base<T> { extra: T; "
      "constructor { super(value: this.extra); } "
      "override read(): T { return identity<T>(this.extra); } } "
      "func main(): int { const value = Derived<int>(extra: 21); "
      "return value.read() + value.value; }");
  ASSERT_TRUE(prepared.Succeeded());
  std::optional<std::uint32_t> main;
  PruneNativeProgram(prepared.program, main);
  runtime::Isolate isolate;
  runtime::LayoutCache cache;
  const auto layouts = runtime::InstantiateLayouts(prepared.program.layouts, isolate, cache);
  EXPECT_FALSE(layouts.classes.empty());
}

TEST(ReachabilityTest, CompactClassesPreserveCreatedObjectsAndStructuralTypeContracts) {
  auto prepared = ir::test::Prepare(R"(
func unused(value: int): () => int { return () => value; }
class Box { value: int; }
func capture(value: int): () => int { return () => value; }
export func main(): int {
  const boxes: Box[] = [Box(value: 21)];
  const values = Map<string, Box>();
  values.set('value', boxes[0]);
  return capture(21)() + (values.get('value') as Box).value;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto original = prepared.program;
  auto& program = prepared.program;
  PruneMain(program);
  EXPECT_LT(program.classes.size(), original.classes.size());
  EXPECT_EQ(program.classes.size(), program.layouts.classes.size());
  for (const auto& function : program.functions) {
    const auto before = std::ranges::find(original.functions, function.name, &ir::Function::name);
    ASSERT_NE(before, original.functions.end());
    std::vector<std::string> expected;
    std::vector<std::string> actual;
    for (const auto& instruction : before->instructions) {
      if (instruction.opcode == Opcode::NewInstance) {
        expected.push_back(original.classes.at(instruction.index).name);
      }
    }
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::NewInstance) {
        actual.push_back(program.classes.at(instruction.index).name);
      }
    }
    EXPECT_EQ(actual, expected) << function.name;
  }
  for (std::size_t index = 0; index < program.layouts.types.size(); ++index) {
    EXPECT_EQ(layout::TypeKey(program.layouts, program.layouts.types[index]),
              layout::TypeKey(original.layouts, original.layouts.types[index]));
  }
  runtime::Isolate isolate;
  runtime::LayoutCache cache;
  const auto layouts = runtime::InstantiateLayouts(program.layouts, isolate, cache);
  EXPECT_EQ(layouts.classes.size(), program.classes.size());
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
  PruneMain(program);
  const auto current = std::ranges::find(program.functions, "used", &ir::Function::name);
  ASSERT_NE(current, program.functions.end());
  EXPECT_NE(current - program.functions.begin(), old_index);
  EXPECT_EQ(current->identity, identity);
}

TEST(ReachabilityTest, ErasedLocalValuesDoNotRetainUnreferencedClassMethods) {
  for (const auto* const expression : {"42", "JSON.parse('42')", "erase(42)", "parsed()"}) {
    SCOPED_TRACE(expression);
    const std::string source =
        "class Unused { value: int = 42; read(): int { return this.value; } } "
        "func erase(value: dynamic): dynamic { return value; } "
        "func parsed(): dynamic { return JSON.parse('42'); } "
        "export func main(): int { const erased: dynamic = " +
        std::string(expression) + "; return erased as int; }";
    auto prepared = ir::test::Prepare(source);
    ASSERT_TRUE(prepared.Succeeded());
    PruneMain(prepared.program);
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
      "export func main(): string { return JSON.stringify(Box()); }",
  });
  for (const auto body : bodies) {
    SCOPED_TRACE(body);
    const std::string source =
        "class Box { value: int = 42; read(): int { return this.value; } "
        "toJson(): int { return this.value; } } class Holder { value: dynamic; } " +
        std::string(body);
    auto prepared = ir::test::Prepare(source);
    ASSERT_TRUE(prepared.Succeeded());
    PruneMain(prepared.program);
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
