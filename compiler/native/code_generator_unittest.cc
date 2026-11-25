#include "compiler/native/code_generator.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <llvm/IR/Module.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Target/TargetMachine.h>

#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/compile_options.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/native_image_data.h"
#include "compiler/native/native_roots.h"
#include "compiler/test_support/preparation_test_support.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::native {
namespace {

TEST(NativeCodeGeneratorTest, OptimizationControlsBothIrAndMachineCodeForEveryPlacement) {
  const auto prepared = ir::test::Prepare("export func answer(): int { return 42; }");
  ASSERT_TRUE(prepared.Succeeded());
  const auto& program = prepared.program;
  const auto calls = PlanNativeCalls(program, NativeCodeTier::Baseline);
  const auto fields = FindFieldTargets(program);
  for (const auto placement : {NativeCodePlacement::Linked, NativeCodePlacement::Dynamic}) {
    for (const auto optimization :
         {NativeOptimization::None, NativeOptimization::Baseline, NativeOptimization::Release}) {
      auto generated =
          Generate(program, calls, fields, "codegen_options", NativeDebugging::Safepoints,
                   NativeCodeTier::Baseline, EscapeOptimization::Enabled, placement, optimization);
      ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
      auto expected = llvm::CodeGenOptLevel::Default;
      if (optimization == NativeOptimization::None) {
        expected = llvm::CodeGenOptLevel::None;
      } else if (optimization == NativeOptimization::Baseline) {
        expected = llvm::CodeGenOptLevel::Less;
      }
      EXPECT_EQ(generated->machine->getOptLevel(), expected);
      const auto finalized = Finalize(*generated);
      ASSERT_TRUE(finalized) << finalized.error();
      if (optimization == NativeOptimization::Release) {
        expected = llvm::CodeGenOptLevel::Aggressive;
      }
      EXPECT_EQ(generated->machine->getOptLevel(), expected);
      const auto object = EmitObject(*generated);
      ASSERT_TRUE(object) << object.error();
      EXPECT_GT((*object)->getBufferSize(), 0U);
    }
  }
}

TEST(NativeCodeGeneratorTest, BaselinePlanNamesMatchRealDefinitions) {
  auto prepared = ir::test::Prepare(
      "func identity(value: int): int { return value; } "
      "export func answer(): int { return identity(42); }");
  ASSERT_TRUE(prepared.Succeeded());
  auto& program = prepared.program;
  program.native_mode = NativeProgramMode::Reloadable;
  const auto plan = PlanBaselineCompilation(program, NativeDebugging::Safepoints,
                                            EscapeOptimization::Enabled, NativeOptimization::None);
  ASSERT_TRUE(plan) << frontend::DiagnosticsText(plan.error());
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    const auto generated = GenerateBaselineFunction(program, *plan, index);
    ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
    for (const auto& name : plan->symbols[index].implementations) {
      const auto* function = generated->module->getFunction(name);
      ASSERT_NE(function, nullptr) << name;
      EXPECT_FALSE(function->isDeclaration()) << name;
    }
  }
}

TEST(NativeImageCodecTest, RoundTripsEmptyAndLengthDelimitedTextWithEmbeddedNullBytes) {
  const std::string embedded("a\0b", 3);
  const std::string boundary(4095, 'x');
  const std::array strings{
      runtime::NativeText{.data = embedded.data(), .size = embedded.size()},
      runtime::NativeText{.data = boundary.data(), .size = boundary.size()},
      runtime::NativeText{},
  };
  runtime::NativeImage image;
  image.strings = {.data = strings.data(), .size = strings.size()};
  const auto encoded = EncodeNativeImage(image);
  const auto decoded = DecodeNativeImage(encoded);
  ASSERT_TRUE(decoded) << decoded.error();
  const auto values = (*decoded)->image()->strings.values();
  ASSERT_EQ(values.size(), strings.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    EXPECT_EQ(values[index].text(), strings[index].text());
  }
  EXPECT_EQ(EncodeNativeImage(*(*decoded)->image()), encoded);
  for (std::size_t size = 0; size < encoded.size(); ++size) {
    EXPECT_FALSE(DecodeNativeImage(std::span(encoded).first(size)));
  }
}

TEST(NativeImageCodecTest, RetainsPhysicalRecordArrayLayouts) {
  const std::array arrays{
      runtime::NativeArrayDescription{.record_fields = 6},
      runtime::NativeArrayDescription{},
  };
  runtime::NativeImage image;
  image.arrays = {.data = arrays.data(), .size = arrays.size()};
  const auto decoded = DecodeNativeImage(EncodeNativeImage(image));
  ASSERT_TRUE(decoded) << decoded.error();
  const auto restored = (*decoded)->image()->arrays.values();
  ASSERT_EQ(restored.size(), arrays.size());
  EXPECT_EQ(restored[0].record_fields, 6U);
  EXPECT_EQ(restored[1].record_fields, 0U);
}

}  // namespace
}  // namespace vela::compiler::native
