#include "compiler/native/call_targets.h"

#include <cstdint>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_roots.h"

namespace vela::compiler::native {
namespace {

ir::Program ScalarVirtualProgram(NativeProgramMode mode) {
  ir::Program program;
  program.native_mode = mode;
  program.selector_declarations = {DeclarationId{1}};
  const frontend::TypeDescriptor receiver{
      .kind = frontend::TypeDescriptor::Kind::Class,
      .name = "Counter",
  };
  const frontend::TypeDescriptor integer{
      .kind = frontend::TypeDescriptor::Kind::Int,
      .name = "int",
  };
  ir::Class owner;
  owner.name = receiver.name;
  owner.dispatch_targets = {1};
  program.classes.push_back(std::move(owner));
  program.functions.resize(2);
  auto& caller = program.functions[0];
  caller.parameter_count = 2;
  caller.register_count = 3;
  caller.return_type = integer;
  caller.value_types = {receiver, integer, integer};
  caller.instructions = {
      {.opcode = Opcode::LoadInt, .dst = 1, .immediate = 1, .location = {}},
      {.opcode = Opcode::CallVirtual, .dst = 2, .lhs = 0, .immediate = 2, .location = {}},
      {.opcode = Opcode::Jump, .index = 1, .location = {}},
      {.opcode = Opcode::Return, .dst = 2, .location = {}},
  };
  auto& method = program.functions[1];
  method.name = "Counter.step";
  method.identity.declaration = DeclarationId{2};
  method.parameter_count = 2;
  method.register_count = 3;
  method.return_type = integer;
  method.value_types = caller.value_types;
  method.signature = CallSignature{
      .parameters = {{.name = "value", .type = 1}},
      .result = 1,
      .hidden_parameters = 1,
      .receiver = 0,
      .parameter_name_order = {0},
  };
  const auto field =
      mode == NativeProgramMode::Reloadable ? std::optional(DeclarationId{3}) : std::nullopt;
  method.instructions = {
      {.opcode = Opcode::SetField, .dst = 0, .lhs = 1, .location = {}, .field_declaration = field},
      {.opcode = Opcode::GetField, .dst = 2, .lhs = 0, .location = {}, .field_declaration = field},
      {.opcode = Opcode::Return, .dst = 2, .location = {}},
  };
  return program;
}

TEST(NativeCallPlanTest, VirtualLoopTargetsRetainBodyDependenciesAndCanonicalInstructions) {
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    const auto program = ScalarVirtualProgram(mode);
    const auto baseline = PlanNativeCalls(program, NativeCodeTier::Baseline);
    EXPECT_TRUE(baseline.direct_calls[0].empty());
    const auto optimized = PlanNativeCalls(program, NativeCodeTier::Optimized);
    const auto& direct = optimized.direct_calls[0];
    ASSERT_EQ(direct.size(), program.functions[0].instructions.size());
    const auto target = direct[1];
    ASSERT_TRUE(target);
    EXPECT_EQ(target->function, 1U);
    EXPECT_FALSE(optimized.IsEntrySlotRequired({.caller = 0, .callee = 1}));
    const auto& dependencies = optimized.dependencies[0];
    ASSERT_EQ(dependencies.size(), 1U);
    EXPECT_EQ(dependencies.front(), 1U);
    EXPECT_EQ(program.functions[0].instructions[1].opcode, Opcode::CallVirtual);
  }
}

TEST(NativeCallPlanTest, ReloadableVirtualSpecializationRequiresLoopAndScalarFieldContracts) {
  auto program = ScalarVirtualProgram(NativeProgramMode::Reloadable);
  program.functions[0].instructions[2] = {.opcode = Opcode::Return, .dst = 2, .location = {}};
  auto plan = PlanNativeCalls(program, NativeCodeTier::Optimized);
  EXPECT_FALSE(plan.direct_calls[0][1]);
  EXPECT_TRUE(plan.dependencies[0].empty());
  EXPECT_TRUE(plan.slot_calls[0].empty());
  program = ScalarVirtualProgram(NativeProgramMode::Reloadable);
  plan = PlanNativeCalls(program, NativeCodeTier::Optimized, NativeDebugging::Instructions);
  EXPECT_TRUE(plan.direct_calls[0].empty());
  program.functions[0].value_types[0].name = "Derived";
  program.classes[0].name = "Derived";
  plan = PlanNativeCalls(program, NativeCodeTier::Optimized);
  EXPECT_TRUE(plan.direct_calls[0].empty());
}

TEST(NativeCallPlanTest, ClosedVirtualTargetsRequireOneImplementationAcrossReceivers) {
  auto program = ScalarVirtualProgram(NativeProgramMode::Closed);
  program.classes.push_back(program.classes.front());
  auto unique = FindUniqueVirtualTargets(program);
  const auto target = unique.front();
  ASSERT_TRUE(target);
  EXPECT_EQ(*target, 1U);
  program.functions.push_back(program.functions.back());
  program.classes.back().dispatch_targets[0] = 2;
  unique = FindUniqueVirtualTargets(program);
  EXPECT_FALSE(unique.front());
  const auto plan = PlanNativeCalls(program, NativeCodeTier::Optimized);
  EXPECT_TRUE(plan.direct_calls[0].empty());
}

TEST(NativeCallPlanTest, ConstructorAndRepeatedEdgesMatchLeafEligibilityAndDebugging) {
  for (const auto parameters : {0U, 64U, 65U, 128U, 129U}) {
    for (const auto debugging : {
             NativeDebugging::Safepoints,
             NativeDebugging::SourceLines,
             NativeDebugging::Instructions,
         }) {
      SCOPED_TRACE(parameters);
      SCOPED_TRACE(static_cast<std::uint8_t>(debugging));
      ir::Program program;
      program.native_mode = NativeProgramMode::Reloadable;
      program.functions.resize(4);
      auto& constructor = program.functions[1];
      constructor.is_generated_constructor = true;
      constructor.parameter_count = parameters;
      constructor.register_count = parameters + 1;
      constructor.return_type.kind = frontend::TypeDescriptor::Kind::Int;
      constructor.value_types.resize(constructor.register_count, constructor.return_type);
      constructor.instructions = {
          {.opcode = Opcode::LoadInt, .dst = parameters, .immediate = 1, .location = {}},
          {.opcode = Opcode::Return, .dst = parameters, .location = {}},
      };
      auto& repeated = program.functions[2];
      repeated = constructor;
      repeated.is_generated_constructor = false;
      program.functions[0].instructions = {
          {.opcode = Opcode::Call, .index = 1, .location = {}},
          {.opcode = Opcode::Call, .index = 2, .location = {}},
          {.opcode = Opcode::Jump, .index = 1, .location = {}},
      };
      program.functions[3] = program.functions[0];
      const auto expected = debugging != NativeDebugging::Safepoints || parameters > 128;
      for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
        const auto plan = PlanNativeCalls(program, tier, debugging);
        for (const auto callee : {1U, 2U}) {
          EXPECT_EQ(plan.IsEntrySlotRequired({.caller = 0, .callee = callee}),
                    tier == NativeCodeTier::Optimized && expected);
          EXPECT_EQ(plan.IsEntrySlotRequired({.caller = 3, .callee = callee}),
                    tier == NativeCodeTier::Optimized && expected);
        }
      }
      const auto leaf = PlanNonrecursiveLeaf(constructor, debugging);
      EXPECT_EQ(leaf, expected ? LeafKind::None
                               : (parameters <= 64 ? LeafKind::Bounded : LeafKind::Guarded));
    }
  }
}

TEST(NativeCallPlanTest, LeafInstructionBudgetsAndBackedgesChooseTheMatchingEntry) {
  for (const auto& [count, expected] : {
           std::pair{2U, LeafKind::Bounded},
           std::pair{64U, LeafKind::Bounded},
           std::pair{65U, LeafKind::Guarded},
           std::pair{128U, LeafKind::Guarded},
           std::pair{129U, LeafKind::None},
       }) {
    SCOPED_TRACE(count);
    ir::Function function;
    function.return_type.kind = frontend::TypeDescriptor::Kind::Int;
    function.register_count = 1;
    function.value_types = {function.return_type};
    function.instructions.resize(count, {.opcode = Opcode::LoadInt, .location = {}});
    function.instructions.back() = {.opcode = Opcode::Return, .location = {}};
    EXPECT_EQ(PlanNonrecursiveLeaf(function, NativeDebugging::Safepoints), expected);
    function.instructions.front() = {.opcode = Opcode::Jump, .index = 0, .location = {}};
    EXPECT_EQ(PlanNonrecursiveLeaf(function, NativeDebugging::Safepoints),
              count > 128 ? LeafKind::None : LeafKind::Guarded);
  }
}

}  // namespace
}  // namespace vela::compiler::native
