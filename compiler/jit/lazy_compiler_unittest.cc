#include "compiler/jit/code_reuse.h"
#include "compiler/jit/compiler.h"

#include <cstddef>
#include <expected>
#include <future>
#include <latch>
#include <memory>
#include <vector>

#include <llvm/Support/MemoryBuffer.h>
#include <string>

#include <gtest/gtest.h>

#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/function_compiler.h"
#include "compiler/jit/test_support.h"
#include "compiler/native/compile_options.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_dispatch.h"

namespace vela::compiler::jit {
namespace {

constexpr CompileOptions kLazyOptions{
    .optimization = NativeOptimization::None,
    .materialization = Materialization::OnDemand,
};

TEST(LazyJitCompilerTest, CompilesOnlyInvokedFunctionsAndReleasesCodeWithItsImage) {
  SourceSession source("main.vela");
  const auto prepared = test::PrepareText(source, R"(
export func answer(): int { return 42; }
export func unused(): int { return 99; }
)");
  ASSERT_TRUE(prepared);
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  auto compiled = (*compiler)->Compile(prepared->snapshot().program, kLazyOptions);
  ASSERT_TRUE(compiled) << frontend::DiagnosticsText(compiled.error());
  EXPECT_EQ((*compiler)->resource_counts().functions, 0U);
  EXPECT_EQ((*compiler)->resource_counts().executable_bytes, 0U);
  {
    runtime::Isolate isolate;
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    const auto answer = isolate.native_dispatch().FindEntry("main.vela", "answer");
    ASSERT_TRUE(answer);
    ASSERT_TRUE(test::ReturnsInteger(*answer, 42));
    EXPECT_EQ((*compiler)->resource_counts().functions, 1U);
    ASSERT_TRUE(test::ReturnsInteger(*answer, 42));
    EXPECT_EQ((*compiler)->resource_counts().functions, 1U);
  }
  compiled->image.reset();
  EXPECT_EQ((*compiler)->resource_counts().functions, 0U);
  EXPECT_EQ((*compiler)->resource_counts().executable_bytes, 0U);
  EXPECT_EQ((*compiler)->resource_counts().batches, 0U);
}

TEST(LazyJitCompilerTest, CapturedUncompiledVersionSurvivesCompilerDestructionAndPublication) {
  SourceSession source("main.vela");
  CodeReuseCache reuse;
  runtime::Isolate isolate;
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto first = test::PrepareText(source, R"(
let count: int = 40;
func increment(): int { count += 1; return count; }
export func answer(): int { return increment(); }
)");
  ASSERT_TRUE(first);
  auto first_code = (*compiler)->Compile(first->snapshot().program, kLazyOptions, &reuse);
  ASSERT_TRUE(first_code) << frontend::DiagnosticsText(first_code.error());
  ASSERT_TRUE(source.Commit(*first));
  ASSERT_TRUE(test::PublishImage(isolate, first_code->image));
  const auto answer = isolate.native_dispatch().FindEntry("main.vela", "answer");
  ASSERT_TRUE(answer);
  const auto captured = answer->Capture();
  ASSERT_TRUE(captured);
  ASSERT_TRUE(test::ReturnsInteger(*answer, 41));
  const auto second = test::PrepareText(source, R"(
let count: int = 0;
func increment(): int { count += 2; return count; }
export func answer(): int { return increment(); }
)");
  ASSERT_TRUE(second);
  auto second_code = (*compiler)->Compile(second->snapshot().program, kLazyOptions, &reuse);
  ASSERT_TRUE(second_code) << frontend::DiagnosticsText(second_code.error());
  ASSERT_TRUE(source.Commit(*second));
  ASSERT_TRUE(test::PublishImage(isolate, second_code->image, 1));
  compiler->reset();
  first_code->image.reset();
  second_code->image.reset();
  const auto current = isolate.native_dispatch().FindEntry("main.vela", "answer");
  ASSERT_TRUE(current);
  ASSERT_TRUE(test::ReturnsInteger(*current, 43));
  const auto old = captured->Invoke({});
  ASSERT_TRUE(old);
  EXPECT_EQ(old->AsInteger(), 45);
}

TEST(LazyJitCompilerTest, RequiresOwnedProgramForLocalOnDemandCompilation) {
  SourceSession source("main.vela");
  const auto prepared = test::PrepareText(source, "export func answer(): int { return 42; }");
  ASSERT_TRUE(prepared);
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled = (*compiler)->Compile(*prepared->snapshot().program, kLazyOptions);
  ASSERT_FALSE(compiled);
  EXPECT_NE(frontend::DiagnosticsText(compiled.error()).find("shared ownership"),
            std::string::npos);
}

TEST(LazyJitCompilerTest, CompilesIndependentObjectsConcurrently) {
  SourceSession source("main.vela");
  const auto prepared = test::PrepareText(source,
                                          "export func first(): int { return 42; } "
                                          "export func second(): int { return 43; } "
                                          "export func third(): int { return 44; }");
  ASSERT_TRUE(prepared);
  auto functions = PrepareFunctions(prepared->snapshot().program, kLazyOptions);
  ASSERT_TRUE(functions) << frontend::DiagnosticsText(functions.error());
  using Object = std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string>;
  std::vector<std::future<Object>> pending;
  pending.reserve((*functions)->symbols.size());
  std::latch start(1);
  for (std::size_t index = 0; index < (*functions)->symbols.size(); ++index) {
    pending.push_back(std::async(std::launch::async, [&, index] {
      start.wait();
      return (*functions)->Compile(index);
    }));
  }
  start.count_down();
  for (auto& task : pending) {
    auto object = task.get();
    ASSERT_TRUE(object) << object.error();
    EXPECT_GT((*object)->getBufferSize(), 0U);
  }
}

}  // namespace
}  // namespace vela::compiler::jit
