#include "compiler/aot/native_compiler.h"

#include <array>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "runtime/native_image.h"

namespace vela::compiler::aot {
namespace {

std::expected<ir::Program, frontend::Diagnostics> Prepare(std::string_view source) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
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
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"main"}});
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
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"empty", "negate"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
}

TEST(NativeCompilerTest, ReferenceLeafFunctionsRegisterRootsOnlyAtFailure) {
  const auto prepared = Prepare(R"(
export func read(values: int[], index: int): int { return values[index]; }
)");
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"read"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  const auto& text = result->llvm_ir;
  EXPECT_EQ(text.find("@vela_native_v1_frame_storage"), std::string::npos);
  EXPECT_EQ(text.find("@vela_native_v1_frame_release"), std::string::npos);
  EXPECT_NE(text.find("@vela_native_v1_leaf_failure"), std::string::npos);
}

TEST(NativeCompilerTest, NullComparisonsUsePresenceForReferencesScalarsAndDynamicValues) {
  const auto prepared = Prepare(R"(
class Box { value: int = 0; }
export func object(value: Box?): bool { return value == null; }
export func text(value: string?): bool { return null == value; }
export func integer(value: int?): bool { return value != null; }
export func erased(value: dynamic): bool { return value == null; }
)");
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"object", "text", "integer", "erased"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_EQ(result->llvm_ir.find("@vela_native_v1_equal"), std::string::npos);
}

TEST(NativeCompilerTest, KnownLocalClosuresCallTheirTypedBodiesDirectlyInsideSort) {
  const auto prepared = Prepare(R"(
export func order(values: int[], direction: int): void {
  values.sort((left, right): int => left > right ? direction : (left < right ? -direction : 0));
}
)");
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"order"}});
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
  ASSERT_TRUE(prepared.has_value());
  const auto result =
      Compile(*prepared, {.exports = {"widen", "truncate", "integer", "decimal", "sum"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  const auto& text = result->llvm_ir;
  EXPECT_EQ(text.find("@vela_native_v1_builtin"), std::string::npos);
  EXPECT_EQ(text.find("@llvm.sadd.with.overflow"), std::string::npos);
  EXPECT_NE(text.find("sitofp i64"), std::string::npos);
  EXPECT_NE(text.find("fptosi double"), std::string::npos);
  EXPECT_NE(text.find("@vela_native_v1_leaf_failure"), std::string::npos);
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(text, diagnostic, context);
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
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.optimization = NativeOptimization::None,
                                        .exports = {"read"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(result->llvm_ir, diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_EQ(result->exports.size(), 1U);
  const auto* body = module->getFunction(
      "vela_aot_body_" + std::to_string(result->exports.front().function));
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
      if (callee != nullptr && callee->getName() == "vela_native_v1_initialize_global") {
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
    ASSERT_TRUE(prepared.has_value());
    const auto result = Compile(*prepared, {.exports = {"sum"}});
    ASSERT_TRUE(result.has_value()) << result.error().front().message;
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    const auto module = llvm::parseAssemblyString(result->llvm_ir, diagnostic, context);
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
    EXPECT_NE(result->llvm_ir.find("@vela_native_v1_leaf_failure"), std::string::npos);
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
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"fields"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(result->llvm_ir, diagnostic, context);
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
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"value"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
}

TEST(NativeCompilerTest, ArrayAppendZeroExtendsLengthsBeforeAddressingElements) {
  const auto prepared = Prepare(R"(
export func append(values: int[], value: int): int { return values.push(value); }
)");
  ASSERT_TRUE(prepared.has_value());
  const auto result =
      Compile(*prepared, {.optimization = NativeOptimization::None, .exports = {"append"}});
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
  ASSERT_TRUE(prepared.has_value());
  const auto private_export = Compile(*prepared, {.exports = {"hidden"}});
  ASSERT_FALSE(private_export.has_value());
  EXPECT_NE(private_export.error().front().message.find("public entry-module"), std::string::npos);
  const auto imported_export = Compile(*prepared, {.exports = {"imported"}});
  ASSERT_FALSE(imported_export.has_value());
}

TEST(NativeCompilerTest, RejectsModuleNamesThatCannotProduceValidCHeaders) {
  const auto prepared = Prepare("export func value(): int { return 42; }");
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.module_name = "invalid-module", .exports = {"value"}});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().front().message.find("C identifier"), std::string::npos);
}

TEST(NativeCompilerTest, EmitsPrivateAsyncMainAndRejectsCustomHostImports) {
  const auto prepared = Prepare("async func main(): Future<int> { return await 42; }");
  ASSERT_TRUE(prepared.has_value());
  const auto result =
      Compile(*prepared, {.exports = {}, .entry_mode = NativeEntryMode::Executable});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_NE(result->llvm_ir.find("define i32 @main()"), std::string::npos);
  EXPECT_NE(result->llvm_ir.find("@vela_native_v1_main"), std::string::npos);
  EXPECT_TRUE(result->exports.empty());

  const auto custom = Prepare("external func host(): int; func main(): int { return host(); }");
  ASSERT_TRUE(custom.has_value());
  const auto rejected =
      Compile(*custom, {.exports = {}, .entry_mode = NativeEntryMode::Executable});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().front().message.find("custom host binding"), std::string::npos);
  EXPECT_TRUE(Compile(*custom).has_value());
}

TEST(NativeCompilerTest, RemovesUnusedImportsAndRetainsOnlySelectedEntryCode) {
  const auto prepared = Prepare(R"(
external func unused(): int;
func removed(): int { return unused(); }
export func answer(): int { return 42; }
)");
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"answer"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  ASSERT_EQ(result->program.functions.size(), 1U);
  EXPECT_EQ(result->program.functions.front().name, "answer");
  EXPECT_EQ(result->exports.front().function, 0U);
  EXPECT_EQ(result->llvm_ir.find("unused"), std::string::npos);
}

}  // namespace
}  // namespace vela::compiler::aot
