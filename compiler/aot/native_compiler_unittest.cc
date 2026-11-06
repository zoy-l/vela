#include "compiler/aot/native_compiler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/SourceMgr.h>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/native_roots.h"
#include "compiler/test_support/preparation_test_support.h"
#include "compiler/test_support/test_sources.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::aot {
namespace {

using ir::test::Prepare;

std::unique_ptr<llvm::Module> ParseModule(std::string_view text, llvm::LLVMContext& context) {
  llvm::SMDiagnostic diagnostic;
  auto module =
      llvm::parseAssemblyString(llvm::StringRef(text.data(), text.size()), diagnostic, context);
  if (module == nullptr) {
    ADD_FAILURE() << diagnostic.getMessage().str();
  }
  return module;
}

TEST(NativeCompilerTest, EmitsOfflineObjectWithScalarCallsAndControlFlow) {
  const auto prepared = Prepare(R"(
func twice(value: int): int { return value * 2; }
export func main(): int {
  let sum = 0;
  for (let index = 0; index < 10; index++) { sum += twice(index); }
  return sum;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"main"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
  EXPECT_EQ(result->target, runtime::NativeTargetIdentity());
  EXPECT_NE(result->llvm_ir.find("vela_aot_entry_"), std::string::npos);
  ASSERT_EQ(result->exports.size(), 1U);
  EXPECT_EQ(result->exports.front().symbol, "vela_aot_export_1");
  EXPECT_NE(result->header.find("VelaAotStatus vela_aot_export_1"), std::string::npos);
  EXPECT_NE(result->header.find("Call_main"), std::string::npos);
  EXPECT_EQ(result->instantiate_symbol, "vela_aot_instantiate");
}

TEST(NativeCompilerTest, EmitsConcreteGenericMethodsAndInterfaceSelectors) {
  for (const auto source : {
           ::vela::compiler::test::kGenericMethodsSource,
           ::vela::compiler::test::kMapLiteralSource,
           ::vela::compiler::test::kMapLiteralAsyncSource,
           ::vela::compiler::test::kWrappedGenericMethodValuesSource,
       }) {
    const auto prepared = Prepare(source);
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto result = Compile(prepared.program, {.exports = {"main"}, .preserve_ir = true});
    ASSERT_TRUE(result) << frontend::DiagnosticsText(result.error());
    EXPECT_FALSE(result->object.empty());
    EXPECT_EQ(result->exports.size(), 1U);
  }
}

TEST(NativeCompilerTest, ExceptionRootCleanupGeneratesLinearCodeSize) {
  constexpr std::array<std::size_t, 2> kCallCounts{16, 64};
  std::array<std::size_t, 2> sizes{};
  for (std::size_t sample = 0; sample < kCallCounts.size(); ++sample) {
    std::string source = R"(
class Box { value: int; }
func identity(value: Box): Box { return value; }
export func main(): int { let total = 0; try {
)";
    for (std::size_t call = 0; call < kCallCounts.at(sample); ++call) {
      source += "total += identity(Box(value: 1)).value;";
    }
    source += "} catch (error: Error) { return -1; } return total; }";
    const auto prepared = Prepare(source);
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto calls = native::PlanNativeCalls(prepared.program, NativeCodeTier::Baseline);
    const auto fields = native::FindFieldTargets(prepared.program);
    const auto generated = native::Generate(
        prepared.program, calls, fields, "root_cleanup", native::NativeDebugging::Safepoints,
        NativeCodeTier::Baseline, native::EscapeOptimization::Disabled);
    ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
    for (const auto& function : *generated->module) {
      sizes.at(sample) += function.getInstructionCount();
    }
  }
  // 调用数扩大四倍时，异常根清理不能随失败点数与根槽数的乘积增长。
  ASSERT_GT(sizes.front(), 0U);
  EXPECT_LT(sizes.back(), sizes.front() * 5);
}

TEST(NativeCompilerTest, EmitsAsyncGenericMethodsWithConcreteExceptionLayouts) {
  const auto prepared = Prepare(::vela::compiler::test::kGenericAsyncMethodsSource);
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto result = Compile(prepared.program, {.exports = {"main"}});
  ASSERT_TRUE(result) << frontend::DiagnosticsText(result.error());
  EXPECT_FALSE(result->object.empty());
}

TEST(NativeCompilerTest, VerifiesBooleanNegationAndEmptyVoidBodies) {
  const auto prepared = Prepare(R"(
export func empty(): void {}
export func negate(value: bool): bool { return !value; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"empty", "negate"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
  EXPECT_TRUE(result->llvm_ir.empty());
}

TEST(NativeCompilerTest, DenseIntegerSwitchAndMatchUseConstantTableDispatch) {
  const auto prepared = Prepare(R"(
export func choose(value: int): int {
  switch (value) {
    case 0: return 13; case 1: return 37; case 2: return 9; case 3: return 81;
    case 4: return 16; case 5: return 62; case 6: return 11; case 7: return 25;
    default: return 0;
  }
}
export func select(value: int): int {
  return match (value) {
    0 => 13, 1 => 37, 2 => 9, 3 => 81, 4 => 16, 5 => 62, 6 => 11, 7 => 25, _ => 0,
  };
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program, {.exports = {"choose", "select"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_NE(result->llvm_ir.find("@switch.table.vela_aot_entry_0"), std::string::npos);
  EXPECT_NE(result->llvm_ir.find("@switch.table.vela_aot_entry_1"), std::string::npos);
}

TEST(NativeCompilerTest, ReferenceLeafFunctionsRegisterRootsOnlyAtFailure) {
  const auto prepared = Prepare(R"(
export func read(values: int[], index: int): int { return values[index]; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"read"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  const auto& text = result->llvm_ir;
  EXPECT_EQ(text.find("@vela_native_v1_frame_storage"), std::string::npos);
  EXPECT_EQ(text.find("@vela_native_v1_frame_release"), std::string::npos);
  EXPECT_NE(text.find("@vela_native_v2_leaf_failure"), std::string::npos);
  EXPECT_EQ(text.find("source.position"), std::string::npos);
}

std::vector<const llvm::Value*> EscapedRuntimeResults(const llvm::Function& function) {
  std::vector<const llvm::Value*> escaped;
  for (const auto& instruction : llvm::instructions(function)) {
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (call != nullptr && call->isIndirectCall() &&
        call->getType()->isIntegerTy(std::numeric_limits<std::uint8_t>::digits)) {
      const auto* output = call->getArgOperand(call->arg_size() - 1);
      escaped.push_back(llvm::getUnderlyingObject(output));
    }
  }
  return escaped;
}

TEST(NativeCompilerTest, KeepsInlinedScalarResultsOutOfEscapedRuntimeScratch) {
  const auto prepared = Prepare(R"(
external func text(): string;
func mix(value: int): int { return (value ^ 42) & 65535; }
export func run(values: int[]): int {
  const retained = text();
  let result = 0;
  for (let index = 0; index < 64; index++) {
    result ^= mix(values[index]) ^ mix(index);
  }
  return result + retained.length;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"run"}, .preserve_ir = true});
  ASSERT_TRUE(result) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  std::size_t checked_calls = 0;
  std::size_t loops_seen = 0;
  for (auto& function : *module) {
    if (function.isDeclaration()) {
      continue;
    }
    const auto escaped = EscapedRuntimeResults(function);
    checked_calls += escaped.size();
    llvm::DominatorTree dominators(function);
    const llvm::LoopInfo loops(dominators);
    for (const auto* loop : loops.getLoopsInPreorder()) {
      ++loops_seen;
      for (const auto* block : loop->blocks()) {
        for (const auto& instruction : *block) {
          const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
          if (store != nullptr && store->getValueOperand()->getType()->isIntegerTy(
                                      std::numeric_limits<std::int64_t>::digits + 1)) {
            const auto* owner = llvm::getUnderlyingObject(store->getPointerOperand());
            EXPECT_EQ(std::ranges::find(escaped, owner), escaped.end()) << result->llvm_ir;
          }
        }
      }
    }
  }
  EXPECT_GT(checked_calls, 0U);
  EXPECT_GT(loops_seen, 0U);
}

bool HasBoundedCheckedArrayLoop(const llvm::Loop& loop) {
  bool has_array_load = false;
  bool has_overflow_check = false;
  for (const auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (load->isAtomic()) {
          return false;
        }
        has_array_load = has_array_load ||
                         load->getType()->isIntegerTy(std::numeric_limits<std::uint64_t>::digits);
      }
      if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        const auto* callee = call->getCalledFunction();
        if (callee == nullptr || !callee->isIntrinsic()) {
          return false;
        }
        has_overflow_check =
            has_overflow_check || callee->getName().starts_with("llvm.sadd.with.overflow");
      }
    }
  }
  return has_array_load && has_overflow_check;
}

TEST(NativeCompilerTest, BoundsPureInlinedCallsInArrayLoops) {
  const auto prepared = Prepare(R"(
func step(value: int): int { return value + 1; }
export func run(values: int[]): int {
  const count = values.length;
  let result = 0;
  for (let index = 0; index < count; index++) { result += step(values[index]); }
  return result;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"run"}, .preserve_ir = true});
  ASSERT_TRUE(result) << frontend::DiagnosticsText(result.error());
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  std::size_t bounded_loops = 0;
  for (auto& function : *module) {
    if (function.isDeclaration()) {
      continue;
    }
    llvm::DominatorTree dominators(function);
    const llvm::LoopInfo loops(dominators);
    for (const auto* loop : loops.getLoopsInPreorder()) {
      if (!loop->getSubLoops().empty()) {
        continue;
      }
      if (HasBoundedCheckedArrayLoop(*loop)) {
        ++bounded_loops;
      }
    }
  }
  EXPECT_GT(bounded_loops, 0U) << result->llvm_ir;
}

TEST(NativeCompilerTest, UsesWordRotationsForProvenLoopValues) {
  const auto prepared = Prepare(R"(
func rotate(value: int, bits: int): int {
  return ((value >>> bits) | (value << (32 - bits))) & 4294967295;
}
export func iterate(seed: int, count: int): int {
  let value = seed & 4294967295;
  for (let i = 0; i < count; i++) {
    value = (rotate(value, 7) + 42) & 4294967295;
  }
  return value;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"iterate"}, .preserve_ir = true});
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->llvm_ir.contains("@llvm.fshr.i32") ||
              result->llvm_ir.contains("@llvm.fshl.i32"))
      << result->llvm_ir;
}

TEST(NativeCompilerTest, VectorizesIndependentObjectFieldUpdates) {
  const auto prepared = Prepare(R"(
export class Particle { x: double; y: double; z: double; vx: double; vy: double; vz: double; }
export func update(points: Particle[]): void {
  for (const point in points) {
    point.x += point.vx;
    point.y += point.vy;
    point.z += point.vz;
  }
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"update"}, .preserve_ir = true});
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->llvm_ir.contains("fadd <2 x double>")) << result->llvm_ir;
}

struct LoopHostCalls {
  std::vector<const llvm::CallBase*> direct;
  std::vector<const llvm::StoreInst*> frame_stores;
  bool has_checked = false;
};

LoopHostCalls CollectHostCalls(const llvm::Loop& loop) {
  LoopHostCalls calls;
  for (const auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call != nullptr && call->isIndirectCall()) {
        if (call->getType()->isIntegerTy(std::numeric_limits<std::int64_t>::digits + 1)) {
          calls.direct.push_back(call);
        } else {
          calls.has_checked = true;
        }
      }
      const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store != nullptr && llvm::getUnderlyingObject(store->getPointerOperand())
                                  ->getName()
                                  .starts_with("import.frame")) {
        calls.frame_stores.push_back(store);
      }
    }
  }
  return calls;
}

TEST(NativeCompilerTest, SelectsDirectOrCheckedHostAbiOutsideTheLoop) {
  const auto prepared = Prepare(R"(
external func step(value: int): int;
export func calculate(count: int): int {
  let result = 0;
  for (let index = 0; index < count; index++) { result = step(result); }
  return result;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"calculate"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  std::size_t direct_loops = 0;
  for (auto& function : *module) {
    if (function.isDeclaration()) {
      continue;
    }
    llvm::DominatorTree dominators(function);
    const llvm::LoopInfo loops(dominators);
    for (const auto* loop : loops.getLoopsInPreorder()) {
      const auto calls = CollectHostCalls(*loop);
      if (calls.direct.empty()) {
        continue;
      }
      ++direct_loops;
      EXPECT_FALSE(calls.has_checked) << result->llvm_ir;
      for (const auto* call : calls.direct) {
        const auto argument_count = call->arg_size();
        EXPECT_TRUE(argument_count == 1U || argument_count == 2U);
        for (const auto* user : call->users()) {
          if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user)) {
            EXPECT_FALSE(loop->contains(store->getParent())) << result->llvm_ir;
          }
        }
      }
      for (const auto* store : calls.frame_stores) {
        // 循环内只重置函数与指令位置；优化器可将相邻的整数常量写入合成向量。
        const auto* value = store->getValueOperand();
        EXPECT_TRUE(llvm::isa<llvm::Constant>(value)) << result->llvm_ir;
        EXPECT_TRUE(value->getType()->isIntOrIntVectorTy()) << result->llvm_ir;
      }
      const auto frame_store_count = calls.frame_stores.size();
      EXPECT_GE(frame_store_count, 1U) << result->llvm_ir;
      EXPECT_LE(frame_store_count, 2U) << result->llvm_ir;
    }
  }
  EXPECT_GT(direct_loops, 0U) << result->llvm_ir;
}

TEST(NativeCompilerTest, NullComparisonsUsePresenceForReferencesScalarsAndDynamicValues) {
  const auto prepared = Prepare(R"(
export class Box { value: int = 0; }
export func object(value: Box?): bool { return value == null; }
export func text(value: string?): bool { return null == value; }
export func integer(value: int?): bool { return value != null; }
export func erased(value: dynamic): bool { return value == null; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(
      prepared.program, {.exports = {"object", "text", "integer", "erased"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_EQ(result->llvm_ir.find("@vela_native_v1_equal"), std::string::npos);
}

TEST(NativeCompilerTest, KnownLocalClosuresCallTheirTypedBodiesDirectlyInsideSort) {
  const auto prepared = Prepare(R"(
export func order(values: int[], direction: int): void {
  values.sort((left, right): int => left > right ? direction : (left < right ? -direction : 0));
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"order"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_EQ(result->llvm_ir.find("@vela_native_v1_call_indirect"), std::string::npos);
  bool direct_comparator = false;
  for (const auto& function : result->program.functions) {
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::Call) {
        const auto& target = result->program.functions.at(instruction.index);
        direct_comparator = direct_comparator || target.parameter_count == 3;
      }
    }
  }
  EXPECT_TRUE(direct_comparator);
}

TEST(NativeCompilerTest, NumericConversionsUseTypedInstructionsWithoutRuntimeDispatch) {
  const auto prepared = Prepare(R"(
export func widen(value: int): double { return value.toDouble(); }
export func truncate(value: double): int { return value.toInt(); }
export func integer(value: int): int { return value.toInt(); }
export func decimal(value: double): double { return value.toDouble(); }
export func sum(values: double[]): double {
  let sum = 0.0;
  for (let index = 0; index < 500; index++) { sum += values[index] / (index + 1).toDouble(); }
  return sum;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program,
              {.exports = {"widen", "truncate", "integer", "decimal", "sum"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  const auto& text = result->llvm_ir;
  EXPECT_EQ(text.find("@vela_native_v1_builtin"), std::string::npos);
  EXPECT_EQ(text.find("@llvm.sadd.with.overflow"), std::string::npos);
  EXPECT_NE(text.find("sitofp i64"), std::string::npos);
  EXPECT_NE(text.find("fptosi double"), std::string::npos);
  EXPECT_NE(text.find("@vela_native_v2_leaf_failure"), std::string::npos);
  llvm::LLVMContext context;
  const auto module = ParseModule(text, context);
  ASSERT_NE(module, nullptr);
  std::size_t loop_blocks = 0;
  for (auto& function : *module) {
    if (function.isDeclaration()) {
      continue;
    }
    llvm::DominatorTree dominators(function);
    const llvm::LoopInfo loops(dominators);
    for (auto* loop : loops.getLoopsInPreorder()) {
      for (const auto* block : loop->blocks()) {
        ++loop_blocks;
        for (const auto& instruction : *block) {
          if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
            const auto* callee = call->getCalledFunction();
            EXPECT_TRUE(callee != nullptr && callee->isIntrinsic()) << text;
          }
          EXPECT_FALSE(llvm::isa<llvm::StoreInst>(instruction)) << text;
        }
      }
    }
  }
  EXPECT_GT(loop_blocks, 0U);
}

TEST(NativeCompilerTest, ReadsReadyGlobalsDirectlyAndInitializesOnlyOnTheOtherBranch) {
  const auto prepared = Prepare(R"(
let count = 42;
export func read(): int { return count; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program,
              {.optimization = NativeOptimization::None, .exports = {"read"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  ASSERT_EQ(result->exports.size(), 1U);
  const auto* body =
      module->getFunction("vela_aot_body_" + std::to_string(result->exports.front().function));
  ASSERT_NE(body, nullptr);
  std::size_t ready_blocks = 0;
  std::size_t initializer_calls = 0;
  for (const auto& block : *body) {
    if (block.getName() == "global.ready") {
      ++ready_blocks;
      for (const auto& instruction : block) {
        EXPECT_FALSE(llvm::isa<llvm::CallBase>(instruction));
      }
    }
    for (const auto& instruction : block) {
      const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call == nullptr) {
        continue;
      }
      const auto* callee = call->getCalledFunction();
      if (callee != nullptr && callee->getName() == "vela_native_v2_initialize_global") {
        ++initializer_calls;
        EXPECT_EQ(block.getName(), "global.initialize");
      }
    }
  }
  EXPECT_EQ(ready_blocks, 1U);
  EXPECT_EQ(initializer_calls, 1U);
}

TEST(NativeCompilerTest, EliminatesIntegerReductionsWithDynamicBounds) {
  const auto sources = std::to_array<std::string_view>({
      "let sum = initial; for (let index = 0; index < bound; index++) { sum += index; }",
      "let sum = initial; for (let index = 0; index > bound; index--) { sum += index; }",
      "let sum = initial; for (let index = 0; index < bound; index++) { sum -= index; }",
      "let sum = initial; for (let index = 0; index < bound; index++) { sum += term; }",
      "let sum = initial; for (let index = 0; index < bound; index++) { sum -= term; }",
      "let sum = initial; for (let index = start; index < bound; index++) { sum += term; }",
      "let sum = initial; for (let index = start; index < 0; index++) { sum += index; }",
      "let sum = initial; for (let index = start; index > 0; index--) { sum += index; }",
      "let sum = initial; for (let index = 0; bound > index; index++) { sum += index; }",
      "let sum = initial; for (let index = start; index < bound; index++) { sum += index; }",
      "let sum = initial; for (let index = start; index > bound; index--) { sum += index; }",
      "let sum = initial; for (let index = start; index < bound; index++) { sum -= index; }",
      "let sum = initial; for (let index = start; index > bound; index--) { sum -= index; }",
  });
  for (const auto source : sources) {
    SCOPED_TRACE(source);
    const auto prepared =
        Prepare("export func sum(bound: int, initial: int, term: int, start: int): int { " +
                std::string(source) + " return sum; }");
    ASSERT_TRUE(prepared.Succeeded());
    const auto result = Compile(prepared.program, {.exports = {"sum"}, .preserve_ir = true});
    ASSERT_TRUE(result.has_value()) << result.error().front().message;
    llvm::LLVMContext context;
    const auto module = ParseModule(result->llvm_ir, context);
    ASSERT_NE(module, nullptr);
    for (auto& function : *module) {
      if (function.isDeclaration()) {
        continue;
      }
      llvm::DominatorTree dominators(function);
      const llvm::LoopInfo loops(dominators);
      EXPECT_TRUE(loops.empty()) << function.getName().str() << '\n' << result->llvm_ir;
    }
    EXPECT_EQ(result->llvm_ir.find("@llvm.sadd.with.overflow"), std::string::npos);
    EXPECT_EQ(result->llvm_ir.find("@llvm.ssub.with.overflow"), std::string::npos);
    EXPECT_NE(result->llvm_ir.find("@vela_native_v2_leaf_failure"), std::string::npos);
  }
}

TEST(NativeCompilerTest, VectorizesBoundedClosedGlobalArrayReductionsAfterInlining) {
  const auto prepared = Prepare(R"(
let values: int[] = [];
export func prepare(count: int, seed: int): void {
  values.clear();
  values.reserve(count);
  for (let index = 0; index < count; index++) { values.push((seed ^ index) & 1023); }
}
func increment(value: int): int { return value + 1; }
export func sum(): int {
  const data = values;
  const count = data.length;
  let total = 0;
  for (let index = 0; index < count; index++) { total += increment(data[index]); }
  return total;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto result =
      Compile(prepared.program, {.exports = {"prepare", "sum"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << frontend::DiagnosticsText(result.error());
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  const auto entry = std::ranges::find(result->exports, "sum", &NativeExport::name);
  ASSERT_NE(entry, result->exports.end());
  const auto* function = module->getFunction("vela_aot_entry_" + std::to_string(entry->function));
  ASSERT_NE(function, nullptr);
  const auto vector_sum =
      std::ranges::any_of(llvm::instructions(function), [](const auto& instruction) {
        const auto* type = instruction.getType();
        return instruction.getOpcode() == llvm::Instruction::Add && type->isVectorTy() &&
               type->getScalarType()->isIntegerTy(std::numeric_limits<std::uint64_t>::digits);
      });
  EXPECT_TRUE(vector_sum) << result->llvm_ir;
}

TEST(NativeCompilerTest, FoldsUnobservedUnitFieldUpdates) {
  const auto prepared = Prepare(R"(
class Box { value: int; }
export func fields(bound: int): int {
  const box = Box(value: 0);
  for (let index = 0; index < bound; index++) { box.value += 1; }
  return box.value;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"fields"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  for (auto& function : *module) {
    if (function.isDeclaration()) {
      continue;
    }
    llvm::DominatorTree dominators(function);
    const llvm::LoopInfo loops(dominators);
    EXPECT_TRUE(loops.empty()) << result->llvm_ir;
  }
}

TEST(NativeCompilerTest, PreservesArrayStorageWhenTemporaryValuesAreErasedToDynamic) {
  const auto prepared = Prepare(R"(
export func value(): dynamic { return [1, 2, 3] as dynamic; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"value"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
}

TEST(NativeCompilerTest, ArrayAppendZeroExtendsLengthsBeforeAddressingElements) {
  const auto prepared = Prepare(R"(
export func append(values: int[], value: int): int { return values.push(value); }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(
      prepared.program,
      {.optimization = NativeOptimization::None, .exports = {"append"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  // uint32 长度的最高位不是符号位；i32 GEP 会将 2 Gi 元素以上的位置符号扩展。
  const auto& text = result->llvm_ir;
  const auto address = text.find("array.element = getelementptr i64,");
  ASSERT_NE(address, std::string::npos);
  const auto line = std::string_view(text).substr(address, text.find('\n', address) - address);
  EXPECT_NE(line.find(", i64 "), std::string_view::npos);
  EXPECT_EQ(line.find(", i32 "), std::string_view::npos);
}

TEST(NativeCompilerTest, ExportsOnlyPublicFunctionsFromTheEntryModule) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'library'; func hidden(): int { return 1; }"},
      {.path = "library.vela", .text = "export func imported(): int { return 2; }"},
  });
  const auto prepared = ir::PrepareParsed(frontend::LoadSources(sources, "main.vela"));
  ASSERT_TRUE(prepared.Succeeded());
  const auto private_export = Compile(prepared.program, {.exports = {"hidden"}});
  ASSERT_FALSE(private_export.has_value());
  EXPECT_NE(private_export.error().front().message.find("public entry-module"), std::string::npos);
  const auto imported_export = Compile(prepared.program, {.exports = {"imported"}});
  ASSERT_FALSE(imported_export.has_value());
}

TEST(NativeCompilerTest, RejectsModuleNamesThatCannotProduceValidCHeaders) {
  const auto prepared = Prepare("export func value(): int { return 42; }");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program, {.module_name = "invalid-module", .exports = {"value"}});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().front().message.find("C identifier"), std::string::npos);
}

TEST(NativeCompilerTest, EmitsPrivateAsyncMainAndRejectsCustomHostImports) {
  const auto prepared = Prepare("async func main(): Future<int> { return await 42; }");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program,
              {.exports = {}, .entry_mode = NativeEntryMode::Executable, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_NE(result->llvm_ir.find("define i32 @main()"), std::string::npos);
  EXPECT_NE(result->llvm_ir.find("@vela_native_v1_main"), std::string::npos);
  EXPECT_TRUE(result->exports.empty());

  const auto custom = Prepare("external func host(): int; func main(): int { return host(); }");
  ASSERT_TRUE(custom.Succeeded());
  const auto rejected =
      Compile(custom.program, {.exports = {}, .entry_mode = NativeEntryMode::Executable});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().front().message.find("custom host binding"), std::string::npos);
  EXPECT_TRUE(Compile(custom.program).has_value());
}

TEST(NativeCompilerTest, RemovesUnusedImportsAndRetainsOnlySelectedEntryCode) {
  const auto prepared = Prepare(R"(
external func unused(): int;
func removed(): int { return unused(); }
export func answer(): int { return 42; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"answer"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  ASSERT_EQ(result->program.functions.size(), 1U);
  EXPECT_EQ(result->program.functions.front().name, "answer");
  EXPECT_EQ(result->exports.front().function, 0U);
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  EXPECT_EQ(module->getFunction("vela_native_v1_import"), nullptr);
}

}  // namespace
}  // namespace vela::compiler::aot
