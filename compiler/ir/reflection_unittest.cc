#include <algorithm>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include "base/callable.h"
#include "base/native_program_mode.h"
#include "base/reflection.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"

namespace vela::ir {
namespace {

TEST(ReflectionPreparationTest, EmitsStablePropertiesLogicalSignaturesAndExplicitSelections) {
  const frontend::SourceInput source{
      .path = "counter.vela",
      .text = R"(
class Base { base: int = 1; }
class Counter extends Base {
  value: int = 10;
  static total: int = 0;
  get amount(): int { return this.value; }
  private set amount(value: int): void { this.value = value; }
  add(value: int): int { return this.value + value; }
}
)",
  };
  const frontend::ReflectionSelection selection{
      .module = "counter.vela",
      .type = *frontend::ParseTypeSource("Counter"),
      .members =
          {
              {
                  .kind = frontend::ReflectionMemberKind::Getter,
                  .name = "amount",
                  .type_arguments = {},
                  .location = {},
              },
              {
                  .kind = frontend::ReflectionMemberKind::Constructor,
                  .name = {},
                  .type_arguments = {},
                  .location = {},
              },
              {
                  .kind = frontend::ReflectionMemberKind::Field,
                  .name = "total",
                  .type_arguments = {},
                  .location = {},
              },
          },
      .source_path = "vela.json",
      .location = {},
      .source = frontend::ParseSourceSnapshot("{}").source,
  };
  auto parsed = frontend::LoadSources(std::span(&source, 1), source.path, std::span(&selection, 1));
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = frontend::TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded());
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    frontend::DeclarationIds identities;
    const auto prepared =
        PrepareProgram(parsed.program, checked.model,
                       {.declarations = &identities, .revision = 1, .native_mode = mode});
    ASSERT_TRUE(prepared.Succeeded());
    const auto& program = prepared.program;
    const auto type =
        std::ranges::find(program.reflection_types, "Counter", &reflection::Type::name);
    ASSERT_NE(type, program.reflection_types.end());
    EXPECT_NE(type->flags & reflection::kRegistered, 0U);
    EXPECT_NE(type->base, kInvalidFunctionTarget);
    const auto& members = program.reflection_members;
    const auto property = std::ranges::find_if(members, [&](const auto& member) {
      return member.declaring_type == type->type && member.kind == reflection::MemberKind::Property;
    });
    ASSERT_NE(property, members.end());
    EXPECT_NE(property->declaration, property->getter);
    EXPECT_NE(property->getter, property->setter);
    EXPECT_NE(property->flags & reflection::kRegistered, 0U);
    const auto setter =
        std::ranges::find(members, property->setter, &reflection::Member::declaration);
    ASSERT_NE(setter, members.end());
    EXPECT_EQ(setter->flags & reflection::kRegistered, 0U);
    EXPECT_NE(setter->flags & reflection::kPrivate, 0U);
    const auto constructor = std::ranges::find_if(members, [&](const auto& member) {
      return member.declaring_type == type->type &&
             member.kind == reflection::MemberKind::Constructor;
    });
    ASSERT_NE(constructor, members.end());
    ASSERT_NE(constructor->target, kInvalidFunctionTarget);
    EXPECT_EQ(constructor->signature.parameters.size(), 1U);
    EXPECT_EQ(constructor->default_parameters.size(), 1U);
    EXPECT_EQ(constructor->signature.hidden_parameters, 0U);
    EXPECT_EQ(program.functions.at(constructor->target).parameter_count, 2U);
    const auto field = std::ranges::find_if(members, [&](const auto& member) {
      return member.declaring_type == type->type && member.name == "total";
    });
    ASSERT_NE(field, members.end());
    EXPECT_NE(field->flags & reflection::kStatic, 0U);
    EXPECT_NE(field->flags & reflection::kRegistered, 0U);
    EXPECT_EQ(field->declaration, program.global_declarations.at(field->target));
  }
}

TEST(ReflectionPreparationTest, EnumSelectionKeepsOriginalOrdinalsAndUnregisteredMembers) {
  const frontend::SourceInput source{
      .path = "colors.vela",
      .text = "enum Color { red, green, blue }",
  };
  const frontend::ReflectionSelection selection{
      .module = "colors.vela",
      .type = *frontend::ParseTypeSource("Color"),
      .members =
          {
              {
                  .kind = frontend::ReflectionMemberKind::EnumMember,
                  .name = "blue",
                  .type_arguments = {},
                  .location = {},
              },
          },
      .source_path = "vela.json",
      .location = {},
      .source = frontend::ParseSourceSnapshot("{}").source,
  };
  const auto prepared = PrepareParsed(
      frontend::LoadSources(std::span(&source, 1), source.path, std::span(&selection, 1)));
  ASSERT_TRUE(prepared.Succeeded());
  const auto& members = prepared.program.reflection_members;
  ASSERT_EQ(members.size(), 3U);
  EXPECT_EQ(members.front().flags & reflection::kRegistered, 0U);
  EXPECT_EQ(members.back().name, "blue");
  EXPECT_EQ(members.back().target, 2U);
  EXPECT_NE(members.back().flags & reflection::kRegistered, 0U);
  EXPECT_NE(members.front().declaration, members.back().declaration);
}

}  // namespace
}  // namespace vela::ir
