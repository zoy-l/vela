#include "compiler/source_session.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "base/declaration.h"
#include "compiler/ir/ir.h"

namespace vela::compiler {
namespace {

const ir::Function* Function(const ir::Program& program, std::string_view name) {
  const auto found = std::ranges::find(program.functions, name, &ir::Function::name);
  return found == program.functions.end() ? nullptr : &*found;
}

const ir::Class* Class(const ir::Program& program, std::string_view name) {
  const auto found = std::ranges::find(program.classes, name, &ir::Class::name);
  return found == program.classes.end() ? nullptr : &*found;
}

DeclarationId Field(const ir::Class& owner, std::string_view name) {
  const auto found = std::ranges::find(owner.field_order, name);
  const auto index = static_cast<std::size_t>(found - owner.field_order.begin());
  return owner.field_declarations.at(index);
}

TEST(SourceSessionTest, PreparingAndDroppingCandidateLeavesTheCommittedSnapshotUntouched) {
  SourceSession session("main.vela");
  const auto baseline = session.snapshot();
  {
    const auto candidate = session.Prepare({
        .changes = {{.path = "main.vela", .text = "func answer(): int { return 42; }"}},
        .added_roots = {},
    });
    ASSERT_TRUE(candidate);
    EXPECT_EQ(candidate->snapshot().revision, 1U);
    EXPECT_EQ(session.snapshot(), baseline);
  }
  EXPECT_EQ(session.snapshot(), baseline);
  EXPECT_TRUE(baseline->identities.records().empty());
}

TEST(SourceSessionTest, FailedCandidateDoesNotConsumeIdentitiesOrReplaceAcceptedSources) {
  SourceSession session("main.vela");
  const auto first = session.Prepare({
      .changes = {{.path = "main.vela", .text = "func answer(): int { return 40; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(first);
  ASSERT_TRUE(session.Commit(*first));
  const auto baseline = session.snapshot();
  const auto invalid = session.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela", .text = "func invalid(): int { return true; }"}},
      .added_roots = {},
  });
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().status, SourceCandidateStatus::SourceError);
  EXPECT_EQ(session.snapshot(), baseline);
  const auto valid = session.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela", .text = "func answer(): int { return 42; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(valid);
  const auto* old_function = Function(baseline->program, "answer");
  const auto* new_function = Function(valid->snapshot().program, "answer");
  ASSERT_NE(old_function, nullptr);
  ASSERT_NE(new_function, nullptr);
  EXPECT_EQ(old_function->identity.declaration, new_function->identity.declaration);
  EXPECT_EQ(baseline->identities.records().size(), valid->snapshot().identities.records().size());
}

TEST(SourceSessionTest, StaleAndForeignCandidatesCannotPublishIntoAnotherBaseline) {
  SourceSession session("main.vela");
  SourceSession other("main.vela");
  const SourceUpdate update{
      .changes = {{.path = "main.vela", .text = "func answer(): int { return 42; }"}},
      .added_roots = {},
  };
  const auto first = session.Prepare(update);
  const auto competing = session.Prepare(update);
  ASSERT_TRUE(first);
  ASSERT_TRUE(competing);
  const auto foreign = other.Commit(*first);
  ASSERT_FALSE(foreign);
  EXPECT_EQ(foreign.error(), SourceCandidateStatus::DifferentSession);
  ASSERT_TRUE(session.Commit(*first));
  const auto stale = session.Commit(*competing);
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error(), SourceCandidateStatus::Stale);
  const auto stale_input = session.Prepare(update);
  ASSERT_FALSE(stale_input);
  EXPECT_EQ(stale_input.error().status, SourceCandidateStatus::Stale);
}

TEST(SourceSessionTest, ReorderedAndInheritedFieldsKeepTheirDeclarationIdentities) {
  SourceSession session("main.vela");
  const auto first = session.Prepare({
      .changes =
          {{.path = "main.vela",
            .text =
                "class Base { first: int = 1; } class Child extends Base { second: int = 2; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(first);
  ASSERT_TRUE(session.Commit(*first));
  const auto old_snapshot = session.snapshot();
  const auto next = session.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela",
                   .text = "class Base { added: int = 3; first: int = 1; } "
                           "class Child extends Base { second: int = 2; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(next);
  const auto* before = Class(old_snapshot->program, "Child");
  const auto* after = Class(next->snapshot().program, "Child");
  ASSERT_NE(before, nullptr);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(before->declaration, after->declaration);
  EXPECT_EQ(Field(*before, "first"), Field(*after, "first"));
  EXPECT_EQ(Field(*before, "second"), Field(*after, "second"));
  const auto* base = Class(next->snapshot().program, "Base");
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(Field(*base, "added"), Field(*after, "added"));
}

TEST(SourceSessionTest, DeletedAndReintroducedDeclarationsNeverReuseTheirOldIdentity) {
  SourceSession session("main.vela");
  const auto first = session.Prepare({
      .changes = {{.path = "main.vela", .text = "func target(): int { return 1; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(first);
  const auto* target = Function(first->snapshot().program, "target");
  ASSERT_NE(target, nullptr);
  const auto original = target->identity.declaration;
  ASSERT_TRUE(session.Commit(*first));
  const auto removed = session.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela", .text = ""}},
      .added_roots = {},
  });
  ASSERT_TRUE(removed);
  ASSERT_TRUE(session.Commit(*removed));
  EXPECT_TRUE(
      session.snapshot()->identities.records()[static_cast<std::size_t>(original.value)].retired);
  const auto restored = session.Prepare({
      .base_revision = 2,
      .changes = {{.path = "main.vela", .text = "func target(): int { return 2; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(restored);
  const auto* replacement = Function(restored->snapshot().program, "target");
  ASSERT_NE(replacement, nullptr);
  EXPECT_NE(replacement->identity.declaration, original);
}

TEST(SourceSessionTest, PrimitiveGenericArgumentsAreIndependentOfBuiltinNamespaceLoading) {
  SourceSession session("main.vela");
  const auto first = session.Prepare({
      .changes = {{.path = "main.vela",
                   .text = "func same<T>(value: T): T { return value; } "
                           "func main(): int { return same<int>(42); }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(first);
  ASSERT_TRUE(session.Commit(*first));
  const auto next = session.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela",
                   .text = "func same<T>(value: T): T { return value; } "
                           "func main(): int { return same<int>(int.parse('42')); }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(next);
  const auto* before = Function(first->snapshot().program, "same<int>");
  const auto* after = Function(next->snapshot().program, "same<int>");
  ASSERT_NE(before, nullptr);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(before->identity.declaration, after->identity.declaration);
}

TEST(SourceSessionTest, ImportAliasesAndLoadOrderDoNotChangeNominalSpecializationIdentities) {
  SourceSession session("main.vela");
  const auto first = session.Prepare({
      .changes =
          {
              {.path = "main.vela",
               .text = "import 'box.vela' as boxes; import 'other.vela' as other; "
                       "func same<T>(value: T): T { return value; } "
                       "func main(): int { return same<boxes.Box>(boxes.Box()).value + "
                       "other.zero(); }"},
              {.path = "box.vela", .text = "export class Box { value: int = 42; }"},
              {.path = "other.vela", .text = "export func zero(): int { return 0; }"},
          },
      .added_roots = {},
  });
  ASSERT_TRUE(first);
  ASSERT_TRUE(session.Commit(*first));
  const auto next = session.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela",
                   .text =
                       "import 'other.vela' as unused; import './nested/../box.vela' as renamed; "
                       "func same<T>(value: T): T { return value; } "
                       "func main(): int { return same<renamed.Box>(renamed.Box()).value; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(next);
  const auto specialized = [](const ir::Function& function) {
    return function.name.starts_with("same<");
  };
  const auto& before = first->snapshot().program.functions;
  const auto& after = next->snapshot().program.functions;
  const auto old_function = std::ranges::find_if(before, specialized);
  const auto new_function = std::ranges::find_if(after, specialized);
  ASSERT_NE(old_function, before.end());
  ASSERT_NE(new_function, after.end());
  EXPECT_NE(old_function->name, new_function->name);
  EXPECT_EQ(old_function->identity.declaration, new_function->identity.declaration);
}

TEST(SourceSessionTest, UninstantiatedGenericDeclarationsKeepIdentitiesWithoutEmittingOldBodies) {
  SourceSession session("main.vela");
  const std::string declarations =
      "func same<T>(value: T): T { return value; } "
      "class Box<T> { value: T; count: int = 1; read(): T { return this.value; } } ";
  const std::string use = "func main(): int { return same<int>(Box<int>(value: 42).read()); }";
  const auto first = session.Prepare({
      .changes = {{.path = "main.vela", .text = declarations + use}},
      .added_roots = {},
  });
  ASSERT_TRUE(first);
  ASSERT_TRUE(session.Commit(*first));
  const auto absent = session.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela", .text = declarations + "func main(): int { return 0; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(absent);
  EXPECT_EQ(Function(absent->snapshot().program, "same<int>"), nullptr);
  EXPECT_EQ(Class(absent->snapshot().program, "Box<int>"), nullptr);
  ASSERT_TRUE(session.Commit(*absent));
  const auto restored = session.Prepare({
      .base_revision = 2,
      .changes = {{.path = "main.vela", .text = declarations + use}},
      .added_roots = {},
  });
  ASSERT_TRUE(restored);
  const auto& before = first->snapshot().identities.records();
  const auto& after = restored->snapshot().identities.records();
  ASSERT_EQ(before.size(), after.size());
  for (std::size_t index = 0; index < before.size(); ++index) {
    EXPECT_EQ(before[index].key, after[index].key);
    EXPECT_FALSE(after[index].retired);
  }
}

TEST(SourceSessionTest, DeletingUninstantiatedTemplateMembersRetiresTheirConcreteIdentities) {
  SourceSession session("main.vela");
  const std::string use = "func main(): int { return Box<int>(value: 42).read(); }";
  const std::string initial =
      "class Box<T> { value: T; count: int = 1; read(): T { return this.value; } } ";
  const auto first = session.Prepare({
      .changes = {{.path = "main.vela", .text = initial + use}},
      .added_roots = {},
  });
  ASSERT_TRUE(first);
  ASSERT_TRUE(session.Commit(*first));
  const auto* old_class = Class(first->snapshot().program, "Box<int>");
  const auto* old_method = Function(first->snapshot().program, "Box<int>.read");
  ASSERT_NE(old_class, nullptr);
  ASSERT_NE(old_method, nullptr);
  const auto removed_field = Field(*old_class, "count");
  const auto removed_method = old_method->identity.declaration;
  const auto removed = session.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela",
                   .text = "class Box<T> { value: T; } func main(): int { return 0; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(removed);
  ASSERT_TRUE(session.Commit(*removed));
  const auto restored = session.Prepare({
      .base_revision = 2,
      .changes = {{.path = "main.vela", .text = initial + use}},
      .added_roots = {},
  });
  ASSERT_TRUE(restored);
  const auto* new_class = Class(restored->snapshot().program, "Box<int>");
  const auto* new_method = Function(restored->snapshot().program, "Box<int>.read");
  ASSERT_NE(new_class, nullptr);
  ASSERT_NE(new_method, nullptr);
  EXPECT_EQ(new_class->declaration, old_class->declaration);
  EXPECT_EQ(Field(*new_class, "value"), Field(*old_class, "value"));
  EXPECT_NE(Field(*new_class, "count"), removed_field);
  EXPECT_NE(new_method->identity.declaration, removed_method);
}

TEST(SourceSessionTest, CompactGlobalAndSelectorIndexesMapToStableDeclarationsAfterReordering) {
  SourceSession session("main.vela");
  const auto first = session.Prepare({
      .changes = {{.path = "main.vela",
                   .text =
                       "let first = 40; let second = 2; "
                       "interface Reader { first(): int; second(): int; } "
                       "func read(value: Reader): int { return value.first() + value.second(); }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(first);
  ASSERT_TRUE(session.Commit(*first));
  const auto next = session.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela",
                   .text =
                       "let second = 2; let added = 0; let first = 40; "
                       "interface Reader { second(): int; first(): int; } "
                       "func read(value: Reader): int { return value.second() + value.first(); }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(next);
  const auto& before = first->snapshot().program;
  const auto& after = next->snapshot().program;
  ASSERT_EQ(before.global_declarations.size(), 2U);
  ASSERT_EQ(after.global_declarations.size(), 3U);
  EXPECT_EQ(before.global_declarations[0], after.global_declarations[2]);
  EXPECT_EQ(before.global_declarations[1], after.global_declarations[0]);
  ASSERT_EQ(before.selector_declarations.size(), 2U);
  ASSERT_EQ(after.selector_declarations.size(), 2U);
  EXPECT_EQ(before.selector_declarations[0], after.selector_declarations[1]);
  EXPECT_EQ(before.selector_declarations[1], after.selector_declarations[0]);
}

TEST(SourceSessionTest, NewRootsAreCheckedEvenWhenNoPreviousModuleImportsThem) {
  SourceSession session("main.vela");
  const auto rejected = session.Prepare({
      .changes = {{.path = "main.vela", .text = ""},
                  {.path = "added.vela", .text = "export func value(): int { return true; }"}},
      .added_roots = {"added.vela"},
  });
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, SourceCandidateStatus::SourceError);
  EXPECT_EQ(session.snapshot()->revision, 0U);
}

TEST(SourceSessionTest, LambdasBelongToTheirOuterVersionAndKeepOldSourceSnapshots) {
  SourceSession session("main.vela");
  const auto first = session.Prepare({
      .changes = {{.path = "main.vela",
                   .text = "func make(): () => int { return (): int => 40; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(first);
  ASSERT_TRUE(session.Commit(*first));
  const auto previous = session.snapshot();
  const auto next = session.Prepare({
      .base_revision = 1,
      .changes =
          {{.path = "main.vela",
            .text =
                "\nfunc make(): () => int { const captured = 42; return (): int => captured; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(next);
  ASSERT_TRUE(session.Commit(*next));
  const auto& old_functions = previous->program.functions;
  const auto& new_functions = next->snapshot().program.functions;
  const auto local = [](const ir::Function& function) { return function.identity.local; };
  const auto old_lambda = std::ranges::find(old_functions, 1U, local);
  const auto new_lambda = std::ranges::find(new_functions, 1U, local);
  ASSERT_NE(old_lambda, old_functions.end());
  ASSERT_NE(new_lambda, new_functions.end());
  EXPECT_EQ(old_lambda->identity.declaration, new_lambda->identity.declaration);
  EXPECT_NE(old_lambda->identity.revision, new_lambda->identity.revision);
  EXPECT_NE(previous->sources.at("main.vela"), session.snapshot()->sources.at("main.vela"));
  EXPECT_TRUE(previous->sources.at("main.vela")->contains("=> 40"));
}

}  // namespace
}  // namespace vela::compiler
