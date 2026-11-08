#include "compiler/native/array_analysis.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::native {
namespace {

TEST(ArrayAnalysisTest, CombinesAliasesBranchesAndEveryLoopWrite) {
  const auto prepared = ir::test::Prepare(R"(
export func sum(value: int, choose: bool): int {
  const values = [1, 2];
  const alias = values;
  if (choose) { alias[0] = value & 255; } else { values[0] = value & 15; }
  for (let index = 0; index < 10; index++) { alias[1] = (values[0] + index) & 255; }
  return alias[1];
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto found = std::ranges::find(program.functions, "sum", &ir::Function::name);
  ASSERT_NE(found, program.functions.end());
  const auto& function = *found;
  const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::FixedStorage);
  const auto& bits =
      facts.generic.at(static_cast<std::size_t>(found - program.functions.begin())).element_bits;
  ASSERT_FALSE(bits.empty());
  std::size_t reads = 0;
  for (const auto& instruction : function.instructions) {
    if (instruction.opcode == Opcode::GetIndex) {
      constexpr std::uint64_t kByteMask = 255;
      EXPECT_EQ(bits.at(instruction.lhs), kByteMask);
      ++reads;
    }
  }
  EXPECT_GT(reads, 0U);
}

TEST(ArrayAnalysisTest, CallsCapturesUnknownAliasesAndUnmaskedWritesRemoveTheBound) {
  for (const std::string_view source : {
           R"(
func alter(values: int[], value: int): void { values[0] = value; }
export func sum(value: int): int {
  const values = [1]; const alias = values;
  alter(alias, value);
  return values[0];
})",
           R"(
export func sum(value: int): int {
  const values = [1];
  const operation = () => { values[0] = value; };
  operation();
  return values[0];
})",
           R"(
class Box { values: int[]; }
func alter(box: Box, value: int): void { box.values[0] = value; }
export func sum(value: int): int {
  const values = [1];
  const box = Box(values: values);
  alter(box, value);
  return values[0];
})",
           R"(
external func input(): int[];
let shared = input();
export func sum(choose: bool): int {
  let values = [1];
  if (choose) { values = shared; }
  return values[0];
})",
           R"(
export func sum(value: int): int {
  const values = [1];
  const alias = values;
  alias[0] = value;
  return values[0];
})",
       }) {
    SCOPED_TRACE(source);
    const auto prepared = ir::test::Prepare(source);
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto& program = prepared.program;
    const auto found = std::ranges::find(program.functions, "sum", &ir::Function::name);
    ASSERT_NE(found, program.functions.end());
    const auto& function = *found;
    const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::FixedStorage);
    const auto& bits =
        facts.generic.at(static_cast<std::size_t>(found - program.functions.begin())).element_bits;
    ASSERT_FALSE(bits.empty());
    std::size_t reads = 0;
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::GetIndex) {
        EXPECT_EQ(bits.at(instruction.lhs), std::numeric_limits<std::uint64_t>::max());
        ++reads;
      }
    }
    EXPECT_GT(reads, 0U);
  }
}

TEST(ArrayAnalysisTest, CombinesClosedGlobalInitializersRebindingsAndCrossFunctionWrites) {
  auto prepared = ir::test::Prepare(R"(
let shared = [1, 2];
let other = shared;
export func prepare(value: int, choose: bool): void {
  shared.clear();
  shared.reserve(4);
  shared.push(value & 255);
  other[0] = value & 511;
  if (choose) { shared = [1023]; }
}
export func sum(): int {
  const alias = shared;
  return alias[0] + other[0];
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  auto& program = prepared.program;
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    program.native_mode = mode;
    const auto analysis_mode = mode == NativeProgramMode::Closed ? ArrayAnalysisMode::FixedStorage
                                                                 : ArrayAnalysisMode::Elements;
    const auto facts = AnalyzeArrayAccess(program, analysis_mode);
    std::size_t reads = 0;
    for (std::size_t index = 0; index < program.functions.size(); ++index) {
      for (const auto& instruction : program.functions.at(index).instructions) {
        if (instruction.opcode == Opcode::GetIndex) {
          const auto expected = mode == NativeProgramMode::Closed
                                    ? std::uint64_t{1023}
                                    : std::numeric_limits<std::uint64_t>::max();
          EXPECT_EQ(facts.generic.at(index).element_bits.at(instruction.lhs), expected);
          ++reads;
        }
      }
    }
    EXPECT_EQ(reads, 2U);
  }
}

TEST(ArrayAnalysisTest, EscapedOrUnknownGlobalAliasesInvalidateEveryReader) {
  for (const std::string_view operation : {
           "export func escape(): int[] { return shared; }",
           "export func alter(value: int): void { shared[0] = value; }",
           "export func replace(values: int[]): void { shared = values; }",
           "external func host(values: int[]): void; export func escape(): void { host(shared); }",
           "export func escape(): dynamic { return shared as dynamic; }",
           "class Box { values: int[]; } export func escape(): Box { return Box(values: shared); }",
           R"(export func capture(value: int): int {
  const f = () => { shared[0] = value; }; f(); return shared[0];
})",
           "export func fill(value: int): void { shared.fill(value); }",
       }) {
    SCOPED_TRACE(operation);
    const auto prepared = ir::test::Prepare(
        "let shared = [1]; let alias = shared; export func sum(): int { return alias[0]; } " +
        std::string(operation));
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto& program = prepared.program;
    const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::FixedStorage);
    std::size_t reads = 0;
    for (std::size_t index = 0; index < program.functions.size(); ++index) {
      for (const auto& instruction : program.functions.at(index).instructions) {
        if (instruction.opcode == Opcode::GetIndex) {
          EXPECT_EQ(facts.generic.at(index).element_bits.at(instruction.lhs),
                    std::numeric_limits<std::uint64_t>::max());
          ++reads;
        }
      }
    }
    EXPECT_GT(reads, 0U);
  }
}

TEST(ArrayAnalysisTest, PropagatesCopiedElementBitsThroughCyclicArrayDependencies) {
  const auto prepared = ir::test::Prepare(R"(
export func sum(): int {
  const first = [1, 2]; const second = [0, 0]; const third = [0, 0];
  for (let index = 0; index < 2; index++) {
    second[index] = first[index]; third[index] = second[index] & 255;
    first[0] = third[1] | 4;
  }
  return first[0] + second[0] + third[0];
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::FixedStorage);
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    for (const auto& instruction : program.functions[index].instructions) {
      if (instruction.opcode == Opcode::GetIndex) {
        EXPECT_EQ(facts.generic[index].element_bits.at(instruction.lhs), 7U);
      }
    }
  }
}

TEST(ArrayAnalysisTest, UnknownGlobalWritesPropagateThroughArrayCopies) {
  const auto prepared = ir::test::Prepare(R"(
let input = [1]; let copy = [0]; let final = [0];
export func update(value: int): void { input[0] = value; }
export func sum(): int {
  copy[0] = input[0]; final[0] = copy[0]; return final[0];
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::FixedStorage);
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    for (const auto& instruction : program.functions[index].instructions) {
      if (instruction.opcode == Opcode::GetIndex) {
        EXPECT_EQ(facts.generic[index].element_bits.at(instruction.lhs),
                  std::numeric_limits<std::uint64_t>::max());
      }
    }
  }
}

TEST(ArrayStorageAnalysisTest, KeepsFreshSnapshotsAndSwappedScratchBuffersFixedUnderElementWrites) {
  const auto prepared = ir::test::Prepare(R"(
export func sum(choose: bool): int {
  const source = [3, 2, 1];
  let first = source.slice(); let second = [0, 0, 0];
  if (choose) { const old = first; first = second; second = old; }
  for (let index = 0; index < 3; index++) { second[index] = first[index]; }
  return second[0] + source[1];
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::FixedStorage);
  std::size_t reads = 0;
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    for (const auto& instruction : program.functions[index].instructions) {
      if (instruction.opcode == Opcode::GetIndex) {
        EXPECT_EQ(facts.generic[index].fixed_storage[instruction.lhs], 1);
        ++reads;
      }
    }
  }
  EXPECT_EQ(reads, 3U);
}

TEST(ArrayStorageAnalysisTest,
     EscapesIncludingErasedAliasesAndStructuralChangesInvalidateTheGroup) {
  for (const std::string_view operation : {
           "values.push(2);",
           "values.reserve(100);",
           "values.clear();",
           "publish(values);",
           "const erased: dynamic = values; publishDynamic(erased);",
           "const capture = () => values; const alias = capture(); alias.push(2);",
           "shared = values;",
           "holder.values = values;",
           "let alias = values; if (choose) { alias = []; } publish(alias);",
           R"(let alias: int[]? = values; if (choose) { alias = null; }
              if (alias != null) { return alias[0]; })",
       }) {
    SCOPED_TRACE(operation);
    const auto prepared = ir::test::Prepare(
        "external func publish(values: int[]): void; external func publishDynamic(value: dynamic): "
        "void; "
        "class Holder { values: int[]; } let shared: int[] = []; let holder = Holder(values: []); "
        "export func sum(choose: bool): int { const values = [1]; " +
        std::string(operation) + " return values[0]; }");
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto& program = prepared.program;
    const auto facts = AnalyzeArrayAccess(program, ArrayAnalysisMode::FixedStorage);
    for (std::size_t index = 0; index < program.functions.size(); ++index) {
      for (const auto& instruction : program.functions[index].instructions) {
        if (instruction.opcode == Opcode::GetIndex) {
          EXPECT_EQ(facts.generic[index].fixed_storage[instruction.lhs], 0);
        }
      }
    }
  }
}

}  // namespace
}  // namespace vela::compiler::native
