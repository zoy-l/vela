#include "compiler/jit/code_cache.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "base/file.h"
#include "base/native_program_mode.h"
#include "base/testing/directory.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/native/code_generator.h"
#include "compiler/project/project_config.h"
#include "compiler/test_support/execution_expectations.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_execution.h"

namespace vela::compiler::jit {
namespace {

class JitCodeCacheTest : public vela::test::DirectoryTest {
 public:
  std::expected<CompiledImage, frontend::Diagnostics> Compile(Compiler& compiler,
                                                              const CompileOptions& options) {
    SourceSession source("main.vela");
    const auto prepared = source.Prepare({
        .changes =
            {
                {
                    .path = "main.vela",
                    .text =
                        "import './dependency.vela' as dependency; let count = dependency.value(); "
                        "export func answer(): int { count += 2; return count; }",
                },
                {.path = "dependency.vela", .text = dependency_},
            },
        .added_roots = {},
    });
    if (!prepared) {
      return std::unexpected(prepared.error().diagnostics);
    }
    auto cached = options;
    cached.code_cache = directory();
    return compiler.Compile(prepared->snapshot().program, cached);
  }
  void SetDependency(std::string source) { dependency_ = std::move(source); }
  static void ExpectImageInteger(std::shared_ptr<const runtime::NativeImage> image,
                                 std::int64_t expected) {
    runtime::Isolate isolate(1);
    auto instance = runtime::NativeInstance::Instantiate(isolate, std::move(image));
    ASSERT_TRUE(instance) << instance.error().message;
    test::ExpectInteger(**instance, expected, "answer");
  }

 private:
  std::string dependency_ = "export func value(): int { return 40; }";
};

TEST_F(JitCodeCacheTest, RelocatesCachedObjectsIntoIndependentLiveBatchesAndFreshIsolates) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto first = Compile(**compiler, {.tier = NativeCodeTier::Optimized});
  ASSERT_TRUE(first);
  EXPECT_EQ(first->cache, CodeCacheStatus::Miss);
  const auto memory = (*compiler)->resource_counts();
  EXPECT_GT(memory.executable_bytes, 0U);
  EXPECT_GT(memory.debug_payload_bytes, 0U);
  EXPECT_EQ(memory.images, 1U);
  EXPECT_GT(memory.image_data_bytes, 0U);
  {
    const auto second = Compile(**compiler, {.tier = NativeCodeTier::Optimized});
    ASSERT_TRUE(second);
    EXPECT_EQ(second->cache, CodeCacheStatus::Hit);
    EXPECT_EQ(second->timings.compilation.count(), 0);
    EXPECT_EQ(second->timings.optimization.count(), 0);
    EXPECT_NE(first->image.get(), second->image.get());
    EXPECT_EQ((*compiler)->resource_counts().batches, 2U);
    EXPECT_EQ((*compiler)->resource_counts().images, 2U);
  }
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
  EXPECT_EQ((*compiler)->resource_counts().images, 1U);
  EXPECT_EQ((*compiler)->resource_counts().image_data_bytes, memory.image_data_bytes);
  EXPECT_EQ((*compiler)->resource_counts().executable_bytes, memory.executable_bytes);
  EXPECT_EQ((*compiler)->resource_counts().read_only_bytes, memory.read_only_bytes);
  EXPECT_EQ((*compiler)->resource_counts().debug_payload_bytes, memory.debug_payload_bytes);
  EXPECT_EQ((*compiler)->resource_counts().constant_payload_bytes, memory.constant_payload_bytes);
  auto another = Compiler::Create();
  ASSERT_TRUE(another);
  const auto third = Compile(**another, {.tier = NativeCodeTier::Optimized});
  ASSERT_TRUE(third);
  EXPECT_EQ(third->cache, CodeCacheStatus::Hit);
  EXPECT_NE(first->image.get(), third->image.get());
  runtime::Isolate left(1);
  runtime::Isolate right(1);
  auto initial_left = left.native_dispatch().Prepare(0, first->image);
  auto initial_right = right.native_dispatch().Prepare(0, third->image);
  ASSERT_TRUE(initial_left);
  ASSERT_TRUE(initial_right);
  ASSERT_TRUE(left.native_dispatch().Publish(std::move(*initial_left)));
  ASSERT_TRUE(right.native_dispatch().Publish(std::move(*initial_right)));
  const auto left_entry = left.native_dispatch().FindEntry("main.vela", "answer");
  const auto right_entry = right.native_dispatch().FindEntry("main.vela", "answer");
  ASSERT_TRUE(left_entry);
  ASSERT_TRUE(right_entry);
  EXPECT_EQ(left_entry->Invoke({})->AsInteger(), 42);
  EXPECT_EQ(left_entry->Invoke({})->AsInteger(), 44);
  another->reset();
  EXPECT_EQ(right_entry->Invoke({})->AsInteger(), 42);
}

TEST_F(JitCodeCacheTest, MapLiteralLayoutsRelocateOnCacheHitInBothTiers) {
  SetDependency(
      "export func value(): int { const data = {'answer': 40}; "
      "return data.get('answer') ?? 0; }");
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    const auto first = Compile(**compiler, {.tier = tier});
    ASSERT_TRUE(first) << frontend::DiagnosticsText(first.error());
    EXPECT_EQ(first->cache, CodeCacheStatus::Miss);
    const auto restored = Compile(**compiler, {.tier = tier});
    ASSERT_TRUE(restored) << frontend::DiagnosticsText(restored.error());
    EXPECT_EQ(restored->cache, CodeCacheStatus::Hit);
    runtime::Isolate isolate(1);
    auto& dispatch = isolate.native_dispatch();
    auto prepared = dispatch.Prepare(0, restored->image);
    ASSERT_TRUE(prepared);
    ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
    const auto entry = dispatch.FindEntry("main.vela", "answer");
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->Invoke({})->AsInteger(), 42);
  }
}

TEST_F(JitCodeCacheTest, DependencyContentsAndCompilationModesHaveDistinctCacheIdentities) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  ASSERT_EQ(Compile(**compiler, {})->cache, CodeCacheStatus::Miss);
  EXPECT_EQ(Compile(**compiler, {})->cache, CodeCacheStatus::Hit);
  EXPECT_EQ(Compile(**compiler, {.optimization = NativeOptimization::None})->cache,
            CodeCacheStatus::Miss);
  EXPECT_EQ(Compile(**compiler, {.tier = NativeCodeTier::Optimized})->cache, CodeCacheStatus::Miss);
  SetDependency("export func value(): int { return 100; }");
  EXPECT_EQ(Compile(**compiler, {})->cache, CodeCacheStatus::Miss);
}

TEST_F(JitCodeCacheTest, CorruptObjectsAreReplacedAndIoErrorsRemainExplicit) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  ASSERT_TRUE(Compile(**compiler, {}));
  const auto path = std::filesystem::directory_iterator(directory())->path();
  const std::string invalid = "incomplete cache write";
  ASSERT_TRUE(WriteFileAtomically(path, std::as_bytes(std::span(invalid))));
  const auto rebuilt = Compile(**compiler, {});
  ASSERT_TRUE(rebuilt);
  EXPECT_EQ(rebuilt->cache, CodeCacheStatus::Invalid);
  EXPECT_EQ(Compile(**compiler, {})->cache, CodeCacheStatus::Hit);
  const auto blocked = WriteCodeCache(path, "entry", llvm::MemoryBufferRef("object", "test"));
  ASSERT_FALSE(blocked);
  EXPECT_NE(blocked.error().find("cannot create JIT code cache"), std::string::npos);
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
}

TEST_F(JitCodeCacheTest, CacheWriteFailureLeavesTheRunningSessionUntouched) {
  const auto cache = directory() / "objects";
  DevelopmentOptions options;
  options.compilation.tier = NativeCodeTier::Optimized;
  options.compilation.code_cache = cache;
  auto session = DevelopmentSession::Create("main.vela", {}, std::move(options));
  ASSERT_TRUE(session);
  auto first = (*session)->Prepare(
      {
          .changes = {{.path = "main.vela", .text = "export func answer(): int { return 42; }"}},
          .added_roots = {},
      },
      DevelopmentOperation::Load);
  ASSERT_TRUE(first);
  (void)(*session)->Submit(std::move(*first));
  ASSERT_EQ((*session)->Pump().status, DevelopmentStatus::Accepted);
  const auto memory = (*session)->code_resources();
  ASSERT_EQ(memory.batches, 1U);
  std::error_code error;
  ASSERT_EQ(cache.parent_path(), directory());
  std::filesystem::remove_all(cache, error);
  ASSERT_FALSE(error);
  const std::string blocked = "cache directory replaced by a file";
  ASSERT_TRUE(WriteFileAtomically(cache, std::as_bytes(std::span(blocked))));
  const auto rejected = (*session)->Prepare(
      {
          .base_revision = 1,
          .changes = {{.path = "main.vela", .text = "export func answer(): int { return 100; }"}},
          .added_roots = {},
      },
      DevelopmentOperation::HotReload);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, DevelopmentStatus::CodeError);
  EXPECT_EQ((*session)->snapshot()->revision, 1U);
  EXPECT_EQ((*session)->code_resources().batches, memory.batches);
  EXPECT_EQ((*session)->code_resources().executable_bytes, memory.executable_bytes);
  EXPECT_EQ((*session)->code_resources().read_only_bytes, memory.read_only_bytes);
  EXPECT_EQ((*session)->code_resources().debug_payload_bytes, memory.debug_payload_bytes);
  EXPECT_EQ((*session)->code_resources().constant_payload_bytes, memory.constant_payload_bytes);
  const auto value = (*session)->Invoke("main.vela", "answer");
  ASSERT_TRUE(value);
  EXPECT_EQ(value->AsInteger(), 42);
}

TEST_F(JitCodeCacheTest, AttributeMetadataSurvivesCacheHitsAndDataChanges) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    for (const auto value : {40, 42}) {
      const auto source =
          "class Tag extends Reflect { value: int; } class Box { [Tag(value: " +
          std::to_string(value) +
          ")] static run(): void {} } "
          "export func answer(): int { return Reflect.findMethods<Tag>()[0].attribute.value; }";
      const frontend::SourceInput input{.path = "main.vela", .text = source};
      for (const auto expected : {CodeCacheStatus::Miss, CodeCacheStatus::Hit}) {
        auto compiler = Compiler::Create();
        ASSERT_TRUE(compiler);
        SourceOptions options;
        options.compilation.tier = tier;
        options.compilation.code_cache =
            directory() / std::to_string(static_cast<std::uint32_t>(tier));
        const auto compiled = CompileModules(**compiler, std::span(&input, 1), input.path, options);
        ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
        EXPECT_EQ(compiled.code.cache, expected);
        ASSERT_NO_FATAL_FAILURE(ExpectImageInteger(compiled.code.image, value));
      }
    }
  }
}

TEST_F(JitCodeCacheTest, ReflectionRegistrationChangesAreVisibleAfterCacheHits) {
  const frontend::SourceInput source{
      .path = "main.vela",
      .text = R"(
class Box { value: int = 42; read(): int { return this.value; } }
export func registered(): bool { return Reflect.typeInfo<Box>().isRegistered; }
export func answer(): int {
  const method = Reflect.typeInfo<Box>().findMethod('read') as MethodInfo;
  return method.invoke(Box(), CallArguments()) as int;
}
)",
  };
  const auto selected = ParseProjectConfiguration("vela.json", R"({"reflection":[
    {"module":"main","type":"Box","members":[{"kind":"method","name":"read"}]}
  ]})");
  ASSERT_TRUE(selected);
  for (const auto registered : {false, true}) {
    for (const auto expected : {CodeCacheStatus::Miss, CodeCacheStatus::Hit}) {
      auto compiler = Compiler::Create();
      ASSERT_TRUE(compiler);
      SourceOptions options;
      options.compilation.code_cache = directory();
      options.compilation.tier = NativeCodeTier::Optimized;
      if (registered) {
        options.reflection = selected->reflection;
      }
      const auto compiled = CompileModules(**compiler, std::span(&source, 1), source.path, options);
      ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
      EXPECT_EQ(compiled.code.cache, expected);
      if (registered) {
        ASSERT_NO_FATAL_FAILURE(ExpectImageInteger(compiled.code.image, 42));
        continue;
      }
      runtime::Isolate isolate(1);
      auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
      ASSERT_TRUE(instance) << instance.error().message;
      const auto result = compiler::test::Execute(**instance, "registered");
      ASSERT_TRUE(result.Succeeded()) << result.message;
      EXPECT_FALSE(result.value.AsBoolean());
    }
  }
}

}  // namespace
}  // namespace vela::compiler::jit
