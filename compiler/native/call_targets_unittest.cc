#include "compiler/native/call_targets.h"

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_roots.h"

namespace vela::compiler::native {
namespace {

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
