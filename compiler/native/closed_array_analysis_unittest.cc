#include "compiler/native/array_analysis.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/integer_range_analysis.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::native {
namespace {

TEST(ClosedArrayAnalysisTest, RefinesBranchesWhoseOtherTargetIsTheFunctionEnd) {
  constexpr std::uint32_t kRegisterCount = 5;
  constexpr std::uint32_t kFunctionEnd = 6;
  ir::Program program;
  auto& function = program.functions.emplace_back();
  function.is_exported = true;
  function.parameter_count = 1;
  function.register_count = kRegisterCount;
  function.value_types.assign(kRegisterCount,
                              {.kind = frontend::TypeDescriptor::Kind::Int, .name = "int"});
  function.value_types[2] = {.kind = frontend::TypeDescriptor::Kind::Bool, .name = "bool"};
  function.instructions = {
      {
          .opcode = Opcode::LoadInt,
          .dst = 1,
          .immediate = std::numeric_limits<std::int64_t>::max(),
          .location = {},
      },
      {.opcode = Opcode::LessInt, .dst = 2, .lhs = 0, .rhs = 1, .location = {}},
      {.opcode = Opcode::JumpIfFalse, .lhs = 2, .index = kFunctionEnd, .location = {}},
      {.opcode = Opcode::LoadInt, .dst = 3, .immediate = 1, .location = {}},
      {.opcode = Opcode::AddInt, .dst = 4, .lhs = 0, .rhs = 3, .location = {}},
      {.opcode = Opcode::Return, .dst = 4, .location = {}},
  };
  const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::Ranges);
  EXPECT_EQ(facts.closed.functions.front().no_overflow[4], 1);
}

TEST(ClosedArrayAnalysisTest, UnaryNegationProofIgnoresUnusedRightOperand) {
  for (const auto value : {std::int64_t{42}, std::numeric_limits<std::int64_t>::min()}) {
    SCOPED_TRACE(value);
    ir::Program program;
    auto& function = program.functions.emplace_back();
    function.register_count = 3;
    function.value_types.assign(3, {.kind = frontend::TypeDescriptor::Kind::Int, .name = "int"});
    function.instructions = {
        {.opcode = Opcode::LoadInt, .dst = 1, .immediate = value, .location = {}},
        {.opcode = Opcode::NegInt, .dst = 2, .lhs = 1, .location = {}},
        {.opcode = Opcode::Return, .dst = 2, .location = {}},
    };
    const std::array<std::uint8_t, 1> roots{1};
    const std::array accesses{ir::AnalyzeRegisterAccesses(function, program.functions)};
    IntegerRangeAnalysis analysis(program, roots, accesses);
    const auto facts = analysis.Run();
    EXPECT_EQ(facts.front().no_overflow[1], value == 42 ? 1 : 0);
  }
}

TEST(ClosedArrayAnalysisTest, ReusesRangeInfrastructureWithoutRetainingPriorElementDomains) {
  constexpr std::uint64_t kElementBits = 15;
  const auto prepared = ir::test::Prepare(R"(
func add(values: int[], choose: bool): int {
  let value = values[0];
  if (choose) { value = value & 15; }
  return pass(value, choose);
}
func pass(value: int, choose: bool): int {
  if (choose) { return value + 1; }
  return value + 1;
}
export func run(values: int[], choose: bool): int { return add(values, choose); }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  std::vector<std::uint8_t> roots(program.functions.size());
  std::vector<std::vector<ir::RegisterAccess>> accesses;
  std::vector<std::vector<std::uint64_t>> bits;
  for (const auto& function : program.functions) {
    roots[accesses.size()] = static_cast<std::uint8_t>(function.is_exported);
    accesses.push_back(ir::AnalyzeRegisterAccesses(function, program.functions));
    bits.emplace_back(function.register_count, kElementBits);
  }
  IntegerRangeAnalysis analysis(program, roots, accesses);
  const auto count_proofs = [](std::span<const IntegerRangeFacts> facts) {
    std::size_t count = 0;
    for (const auto& function : facts) {
      count += static_cast<std::size_t>(std::ranges::count(function.no_overflow, 1));
    }
    return count;
  };
  EXPECT_EQ(count_proofs(analysis.Run()), 0U);
  EXPECT_EQ(count_proofs(analysis.Run(bits)), 2U);
  EXPECT_EQ(count_proofs(analysis.Run()), 0U);
}

TEST(ClosedArrayAnalysisTest, HandledSynchronousCallsInvalidateTheirEntireAliasGroup) {
  const auto prepared = ir::test::Prepare(R"(
func touch(values: int[]): void {
  try { values[0] = 7; } catch (error: Error) { return; }
}
func read(values: int[]): int { return values[0]; }
export func run(): int { const values = [1]; touch(values); return read(values); }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::Ranges).closed;
  const auto read = std::ranges::find(program.functions, "read", &ir::Function::name);
  ASSERT_NE(read, program.functions.end());
  const auto index = static_cast<std::size_t>(read - program.functions.begin());
  EXPECT_EQ(facts.functions[index].element_bits[0], std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(facts.functions[index].fixed_storage[0], 0);
}

TEST(ClosedArrayAnalysisTest, CarriesRangesAndArrayWritesThroughMutualRecursion) {
  const auto prepared = ir::test::Prepare(R"(
func inspect(values: int[], row: int, column: int): bool {
  for (let index = 0; index < row; index++) {
    const value = values[index];
    if (value - index == column - row || value + index == column + row) { return false; }
  }
  return true;
}
func first(values: int[], row: int): int {
  if (row == 6) { return 1; }
  for (let column = 0; column < 6; column++) {
    if (inspect(values, row, column)) {
      values[row] = column;
      return second(values, row + 1);
    }
  }
  return 0;
}
func second(values: int[], row: int): int { return first(values, row); }
export func run(): int { return first([0, 0, 0, 0, 0, 0], 0); }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::Ranges).closed;
  std::size_t reads = 0;
  std::size_t writes = 0;
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions[index];
    if (function.name == "run") {
      continue;
    }
    const auto& result = facts.functions[index];
    ASSERT_NE(facts.reachable[index], 0);
    EXPECT_EQ(facts.roots[index], 0);
    EXPECT_EQ(result.element_bits[0], 7U);
    EXPECT_EQ(result.fixed_storage[0], 1);
    for (std::size_t position = 0; position < function.instructions.size(); ++position) {
      const auto opcode = function.instructions[position].opcode;
      if (opcode == Opcode::GetIndex || opcode == Opcode::SetIndex) {
        EXPECT_EQ(result.in_bounds[position], 1) << function.name << ':' << position;
        reads += static_cast<std::size_t>(opcode == Opcode::GetIndex);
        writes += static_cast<std::size_t>(opcode == Opcode::SetIndex);
      }
      if (opcode == Opcode::AddInt || opcode == Opcode::SubInt) {
        EXPECT_EQ(result.no_overflow[position], 1) << function.name << ':' << position;
      }
    }
  }
  EXPECT_EQ(reads, 1U);
  EXPECT_EQ(writes, 1U);
}

TEST(ClosedArrayAnalysisTest, MergesEveryCallerLengthAndKeepsUnknownWritesChecked) {
  const auto prepared = ir::test::Prepare(R"(
func read(values: int[], index: int): int { return values[index] + 1; }
func write(values: int[], value: int): void { values[0] = value; }
export func small(): int { return read([1], 1); }
export func large(value: int): int {
  const values = [1, 2, 3]; write(values, value); return read(values, 1);
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::Ranges).closed;
  const auto found = std::ranges::find(program.functions, "read", &ir::Function::name);
  ASSERT_NE(found, program.functions.end());
  const auto& result = facts.functions[static_cast<std::size_t>(found - program.functions.begin())];
  EXPECT_EQ(result.element_bits[0], std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(result.fixed_storage[0], 1);
  for (std::size_t position = 0; position < found->instructions.size(); ++position) {
    const auto opcode = found->instructions[position].opcode;
    if (opcode == Opcode::GetIndex) {
      EXPECT_EQ(result.in_bounds[position], 0);
    }
    if (opcode == Opcode::AddInt) {
      EXPECT_EQ(result.no_overflow[position], 0);
    }
  }
}

TEST(ClosedArrayAnalysisTest, ExternalAccessAndStructuralMutationInvalidateArrayProofs) {
  for (const std::string_view operation : {
           "values.clear();",
           "values.reserve(20);",
           "publish(values);",
           "saved = values;",
           "const callback = () => values; callback();",
       }) {
    SCOPED_TRACE(operation);
    const auto prepared = ir::test::Prepare(
        "external func publish(values: int[]): void; let saved: int[] = []; "
        "func read(values: int[]): int { return values[0]; } "
        "func change(values: int[]): void { " +
        std::string(operation) +
        " } export func run(): int { const values = [1]; change(values); return read(values); }");
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto& program = prepared.program;
    const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::Ranges).closed;
    const auto found = std::ranges::find(program.functions, "read", &ir::Function::name);
    ASSERT_NE(found, program.functions.end());
    const auto& result =
        facts.functions[static_cast<std::size_t>(found - program.functions.begin())];
    EXPECT_EQ(result.fixed_storage[0], 0);
    for (std::size_t position = 0; position < found->instructions.size(); ++position) {
      if (found->instructions[position].opcode == Opcode::GetIndex) {
        EXPECT_EQ(result.in_bounds[position], 0);
      }
    }
  }
}

TEST(ClosedArrayAnalysisTest, ExportedAndAddressTakenParametersRemainUnrestricted) {
  const auto prepared = ir::test::Prepare(R"(
func indirect(values: int[], index: int): int { return values[index] + 1; }
export func direct(values: int[], index: int): int { return values[index] + 1; }
export func run(): int { const callback = indirect; return callback([1], 0) + direct([1], 0); }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::Ranges).closed;
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto& function = program.functions[index];
    if (function.name != "indirect" && function.name != "direct") {
      continue;
    }
    EXPECT_EQ(facts.roots[index], 1);
    const auto& result = facts.functions[index];
    EXPECT_TRUE(facts.parameters[index][1].isFullSet());
    EXPECT_EQ(result.element_bits[0], std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(result.fixed_storage[0], 0);
  }
}

}  // namespace
}  // namespace vela::compiler::native
