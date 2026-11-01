#include "compiler/native/array_analysis.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string_view>

#include <gtest/gtest.h>

#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "compiler/ir/preparation_test_support.h"

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
  const auto bits = FindPrivateArrayElementBits(function, program);
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
let shared = [1];
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
    const auto prepared = ir::test::Prepare(source);
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto& program = prepared.program;
    const auto found = std::ranges::find(program.functions, "sum", &ir::Function::name);
    ASSERT_NE(found, program.functions.end());
    const auto& function = *found;
    const auto bits = FindPrivateArrayElementBits(function, program);
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

TEST(ArrayAnalysisTest, InspectShaRanges) {
  const auto prepared = ir::PrepareParsed(frontend::LoadProgram(
      "C:/Users/zoy/Documents/Vela/benchmarks/dasprofile/workloads.vela"));
  ASSERT_TRUE(prepared.Succeeded());
  const auto& program = prepared.program;
  const auto found = std::ranges::find(program.functions, "shaDigest", &ir::Function::name);
  ASSERT_NE(found, program.functions.end());
  const auto bits = FindPrivateArrayElementBits(*found, program);
  std::ofstream facts("C:/Users/zoy/AppData/Local/Temp/vela-sha-array-facts.txt");
  for (const auto& instruction : found->instructions) {
    if (instruction.opcode == Opcode::GetIndex) {
      facts << instruction.lhs << ' ' << bits.at(instruction.lhs) << '\n';
    }
  }
  const auto compiled = aot::Compile(program, {.exports = {"prepare", "sha256"}, .preserve_ir = true});
  ASSERT_TRUE(compiled);
  std::ofstream output("C:/Users/zoy/AppData/Local/Temp/vela-sha-private-array.ll");
  output << compiled->llvm_ir;
}

}  // namespace
}  // namespace vela::compiler::native
