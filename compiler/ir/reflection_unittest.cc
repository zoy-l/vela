#include <algorithm>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/declaration.h"
#include "base/member.h"
#include "base/native_program_mode.h"
#include "base/reflection.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "compiler/test_support/reflection_test_support.h"

namespace vela::ir {
namespace {

using compiler::test::SelectMember;
using compiler::test::SelectReflection;
using SelectionKind = frontend::ReflectionMemberKind;

TEST(ReflectionPreparationTest, ImmediateArgumentRecordsBecomeRootedValueWindows) {
  const frontend::SourceInput source{
      .path = "main.vela",
      .text = R"(
export func snapshot(method: MethodInfo<dynamic>, receiver: dynamic, value: int): dynamic {
  return method.invoke(receiver, CallArguments(items: [CallArgument(value: value)]));
}
)",
  };
  auto parsed = frontend::LoadSources(std::span(&source, 1), source.path);
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = frontend::TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded());
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    frontend::DeclarationIds identities;
    const auto prepared = PrepareProgram(parsed.program, checked.model,
                                         {.declarations = &identities, .native_mode = mode});
    ASSERT_TRUE(prepared.Succeeded());
    const auto& functions = prepared.program.functions;
    const auto function = std::ranges::find_if(
        functions, [](const auto& candidate) { return candidate.name.ends_with("snapshot"); });
    ASSERT_NE(function, functions.end());
    const auto& instructions = function->instructions;
    EXPECT_TRUE(std::ranges::none_of(instructions, [](const auto& instruction) {
      return instruction.opcode == Opcode::NewInstance || instruction.opcode == Opcode::NewArray ||
             instruction.construction.has_value() || instruction.completes_construction;
    }));
    const auto invocation = std::ranges::find_if(instructions, [](const auto& instruction) {
      return instruction.opcode == Opcode::BuiltinCall &&
             instruction.index == static_cast<std::uint32_t>(Builtin::ReflectionInvoke);
    });
    ASSERT_NE(invocation, instructions.end());
    const auto access = AnalyzeRegisters(*invocation, functions);
    EXPECT_EQ(access.argument_count, kReflectionInvokeHeaderArity + 2);
  }
}

TEST(ReflectionPreparationTest, EmitsStablePropertiesLogicalSignaturesAndExplicitSelections) {
  const frontend::SourceInput source{
      .path = "counter.vela",
      .text = R"(
class Base { base: int = 1; }
class Counter extends Base {
  value: int = 10;
  static total: int = 0;
  get amount(): int { return this.value; }

  set #amount(value: int): void { this.value = value; }

  add(value: int): int { return this.value + value; }
}
)",
  };
  const auto selection = SelectReflection("Counter",
                                          {
                                              SelectMember(SelectionKind::Getter, "amount"),
                                              SelectMember(SelectionKind::Constructor),
                                              SelectMember(SelectionKind::Field, "total"),
                                          },
                                          source.path);
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
      return member.declaring_type == type->type &&
             member.kind == reflection::MemberKind::Property && member.name == "amount";
    });
    ASSERT_NE(property, members.end());
    EXPECT_EQ(property->flags & reflection::kVirtual, 0U);
    EXPECT_NE(property->declaration, property->getter);
    EXPECT_EQ(property->setter, DeclarationId{});
    EXPECT_NE(property->flags & reflection::kRegistered, 0U);
    const auto setter = std::ranges::find_if(members, [&](const auto& member) {
      return member.declaring_type == type->type && member.name == "#amount" &&
             member.kind == reflection::MemberKind::Setter;
    });
    ASSERT_NE(setter, members.end());
    EXPECT_EQ(setter->flags & reflection::kRegistered, 0U);
    EXPECT_NE(setter->flags & reflection::kPrivate, 0U);
    const auto& dynamic_members = program.members;
    const auto dynamic_property = std::ranges::find_if(dynamic_members, [&](const auto& member) {
      return member.receiver_type == type->type && member.kind == MemberKind::Property &&
             member.name == "amount";
    });
    ASSERT_NE(dynamic_property, dynamic_members.end());
    EXPECT_NE(dynamic_property->target, kInvalidFunctionTarget);
    EXPECT_FALSE(dynamic_property->setter);
    const auto constructor = std::ranges::find_if(members, [&](const auto& member) {
      return member.declaring_type == type->type &&
             member.kind == reflection::MemberKind::Constructor;
    });
    ASSERT_NE(constructor, members.end());
    ASSERT_NE(constructor->target, kInvalidFunctionTarget);
    ASSERT_EQ(constructor->signature.parameters.size(), 2U);
    EXPECT_EQ(constructor->signature.parameters[0].name, "base");
    EXPECT_EQ(constructor->signature.parameters[1].name, "value");
    EXPECT_TRUE(constructor->signature.parameters[0].has_default);
    EXPECT_TRUE(constructor->signature.parameters[1].has_default);
    EXPECT_EQ(constructor->signature.hidden_parameters, 0U);
    EXPECT_EQ(program.functions.at(constructor->target).parameter_count, 4U);
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
  const auto selection =
      SelectReflection("Color", {SelectMember(SelectionKind::EnumMember, "blue")}, source.path);
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
