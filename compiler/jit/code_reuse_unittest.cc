#include "compiler/jit/code_reuse.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {
namespace {

using frontend::DiagnosticsText;

class JitCodeFanoutTest : public testing::TestWithParam<std::uint32_t> {};

TEST_P(JitCodeFanoutTest, EditsReplaceOneTypedSlotAndPreserveEveryCaller) {
  const auto count = GetParam();
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  SourceSession source("main.vela");
  CodeReuseCache reuse;
  runtime::Isolate isolate(1);
  std::vector<runtime::NativeEntry> entries;
  runtime::NativeTypedEntry target = nullptr;
  for (std::int64_t version = 1; version <= 3; ++version) {
    auto text = "func value(): int { return " + std::to_string(version) + "; }\n";
    for (std::size_t index = 1; index < count; ++index) {
      text += "export func caller" + std::to_string(index) + "(): int { return value(); }\n";
    }
    const auto prepared = test::PrepareText(source, text);
    ASSERT_TRUE(prepared) << DiagnosticsText(prepared.error().diagnostics);
    const auto compiled =
        (*compiler)->Compile(prepared->snapshot().program,
                             {.tier = NativeCodeTier::Optimized, .preserve_ir = true}, &reuse);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    const auto functions = compiled->image->functions.values();
    const auto value = std::ranges::find(functions, "value",
                                         [](const auto& function) { return function.name.text(); });
    ASSERT_NE(value, functions.end());
    ASSERT_NE(value->typed_entry, nullptr);
    if (version == 1) {
      EXPECT_NE(compiled->llvm_ir.find("call.typed.entry"), std::string::npos);
      for (const auto& function : functions) {
        entries.push_back(function.entry);
      }
    } else {
      EXPECT_EQ(compiled->compiled_functions, 1U);
      EXPECT_NE(value->typed_entry, target);
      for (std::size_t index = 0; index < functions.size(); ++index) {
        const auto& function = functions.subspan(index).front();
        if (function.name.text().starts_with("caller")) {
          EXPECT_EQ(function.entry, entries.at(index));
        }
      }
    }
    target = value->typed_entry;
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image, source.snapshot()->revision));
    ASSERT_TRUE(source.Commit(*prepared));
    for (std::size_t index = 1; index < count; ++index) {
      const auto entry =
          isolate.native_dispatch().FindEntry("main.vela", "caller" + std::to_string(index));
      ASSERT_TRUE(entry);
      ASSERT_TRUE(test::ReturnsInteger(*entry, version));
    }
    isolate.heap().Collect();
    EXPECT_LE((*compiler)->resource_counts().batches, 2U);
    EXPECT_EQ((*compiler)->resource_counts().images, 1U);
  }
}

INSTANTIATE_TEST_SUITE_P(ProjectSizes, JitCodeFanoutTest, testing::Values(32U, 256U));

TEST(JitCodeReuseTest, RecursiveComponentEditsDoNotInvalidateItsOutsideCaller) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  SourceSession source("main.vela");
  CodeReuseCache reuse;
  runtime::Isolate isolate(1);
  runtime::NativeEntry caller = nullptr;
  for (std::int64_t version = 1; version <= 3; ++version) {
    const auto prepared = test::PrepareText(
        source, "func even(n: int): int { if (n == 0) { return " + std::to_string(version) +
                    "; } return odd(n - 1); }\n"
                    "func odd(n: int): int { if (n == 0) { return 0; } return even(n - 1); }\n"
                    "export func answer(): int { return even(2); }");
    ASSERT_TRUE(prepared) << DiagnosticsText(prepared.error().diagnostics);
    const auto compiled = (*compiler)->Compile(prepared->snapshot().program,
                                               {.tier = NativeCodeTier::Optimized}, &reuse);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    const auto functions = compiled->image->functions.values();
    const auto answer = std::ranges::find(
        functions, "answer", [](const auto& function) { return function.name.text(); });
    ASSERT_NE(answer, functions.end());
    if (version != 1) {
      EXPECT_EQ(compiled->compiled_functions, 2U);
      EXPECT_EQ(answer->entry, caller);
    }
    caller = answer->entry;
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image, source.snapshot()->revision));
    ASSERT_TRUE(source.Commit(*prepared));
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", "answer");
    ASSERT_TRUE(entry);
    ASSERT_TRUE(test::ReturnsInteger(*entry, version));
  }
}

TEST(JitCodeReuseTest, CapturedCallerKeepsItsBodyAndResolvesEditedAndDeletedTargets) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  SourceSession source("main.vela");
  CodeReuseCache reuse;
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  std::optional<runtime::NativeVersionEntry> captured;
  const std::array sources{
      "func value(): int { return 1; } export func answer(): int { return value(); }",
      "func value(): int { return 2; } export func answer(): int { return value(); }",
      "func value(): int { return 3; } export func answer(): int { return value() + 100; }",
      "export func replacement(): int { return 0; }",
  };
  for (std::size_t index = 0; index < sources.size(); ++index) {
    const auto prepared = test::PrepareText(source, sources.at(index));
    ASSERT_TRUE(prepared) << DiagnosticsText(prepared.error().diagnostics);
    const auto compiled = (*compiler)->Compile(prepared->snapshot().program,
                                               {.tier = NativeCodeTier::Optimized}, &reuse);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image, source.snapshot()->revision));
    ASSERT_TRUE(source.Commit(*prepared));
    if (index == 0) {
      const auto entry = dispatch.FindEntry("main.vela", "answer");
      ASSERT_TRUE(entry);
      const auto version = entry->Capture();
      ASSERT_TRUE(version);
      captured = *version;
    }
    if (!captured.has_value()) {
      FAIL() << "Expected captured to have a value";
    }
    const auto result = captured.value().Invoke({});
    if (index + 1 == sources.size()) {
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error(), ExecutionStatus::TargetDeleted);
    } else {
      ASSERT_TRUE(result);
      EXPECT_EQ(result->AsInteger(), static_cast<std::int64_t>(index + 1));
    }
  }
  captured.reset();
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
}

TEST(JitCodeReuseTest, ConstructorBodySpecializationInvalidatesItsCallerAcrossAnEntrySlot) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  SourceSession source("main.vela");
  CodeReuseCache reuse;
  runtime::Isolate isolate(1);
  const auto prepared = test::PrepareText(
      source,
      "class Point { x: int; } "
      "export func answer(): int { const point = Point(x: 42); return point.x; }");
  ASSERT_TRUE(prepared) << DiagnosticsText(prepared.error().diagnostics);
  auto program = prepared->snapshot().program;
  const auto first = (*compiler)->Compile(program, {.tier = NativeCodeTier::Optimized}, &reuse);
  ASSERT_TRUE(first) << DiagnosticsText(first.error());
  const auto constructor = std::ranges::find_if(
      program.functions, [](const auto& function) { return function.is_generated_constructor; });
  ASSERT_NE(constructor, program.functions.end());
  const auto answer = std::ranges::find(program.functions, "answer",
                                        [](const auto& function) { return function.name; });
  ASSERT_NE(answer, program.functions.end());
  const auto constructor_index = static_cast<std::size_t>(constructor - program.functions.begin());
  const auto answer_index = static_cast<std::size_t>(answer - program.functions.begin());
  const auto& dependencies = reuse.sources.at(answer_index).body_dependencies;
  EXPECT_NE(std::ranges::find(dependencies, constructor_index), dependencies.end());
  EXPECT_EQ(first->function_metrics.at(answer_index).eliminated_allocations, 1U);
  ASSERT_TRUE(test::PublishImage(isolate, first->image));
  // 仅改变构造函数的调试位置，隔离调用者本身及类布局的指纹变化。
  ASSERT_FALSE(constructor->instructions.empty());
  ++constructor->instructions.front().location.column;
  ++program.revision;
  for (auto& function : program.functions) {
    function.identity.revision = program.revision;
  }
  const auto second = (*compiler)->Compile(program, {.tier = NativeCodeTier::Optimized}, &reuse);
  ASSERT_TRUE(second) << DiagnosticsText(second.error());
  EXPECT_EQ(second->compiled_functions, 2U);
  ASSERT_TRUE(test::PublishImage(isolate, second->image, first->image->revision));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "answer");
  ASSERT_TRUE(entry);
  ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
}

}  // namespace
}  // namespace vela::compiler::jit
