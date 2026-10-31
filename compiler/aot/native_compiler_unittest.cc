#include "compiler/aot/native_compiler.h"

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
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/SourceMgr.h>

#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "compiler/ir/preparation_test_support.h"
#include "compiler/native/code_generator.h"
#include "runtime/native_image.h"

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

struct LoopHostCalls {
  std::vector<const llvm::CallBase*> direct;
  bool has_checked = false;
};

LoopHostCalls CollectHostCalls(const llvm::Loop& loop) {
  LoopHostCalls calls;
  for (const auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call == nullptr || !call->isIndirectCall()) {
        continue;
      }
      if (call->getType()->isIntegerTy(std::numeric_limits<std::int64_t>::digits + 1)) {
        calls.direct.push_back(call);
      } else {
        calls.has_checked = true;
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
      for (const auto* call : calls.direct) {
        EXPECT_TRUE(call->arg_size() == 1U || call->arg_size() == 2U);
      }
      if (!calls.direct.empty()) {
        ++direct_loops;
        EXPECT_FALSE(calls.has_checked) << result->llvm_ir;
      }
    }
  }
  EXPECT_GT(direct_loops, 0U) << result->llvm_ir;
}

TEST(NativeCompilerTest, NullComparisonsUsePresenceForReferencesScalarsAndDynamicValues) {
  const auto prepared = Prepare(R"(
class Box { value: int = 0; }
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

TEST(NativeCompilerTest, EliminatesMonotoneIntegerReductionsWithDynamicBounds) {
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
