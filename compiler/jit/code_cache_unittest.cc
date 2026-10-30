#include "compiler/jit/code_cache.h"

#include <chrono>
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
#include "compiler/development_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/native/code_generator.h"
#include "compiler/source_session.h"
#include "runtime/isolate.h"
#include "runtime/native_dispatch.h"

namespace vela::compiler::jit {
namespace {

class JitCodeCacheTest : public testing::Test {
 public:
  void SetUp() override {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    ASSERT_FALSE(error);
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = temporary / ("vela-jit-cache-" + std::to_string(suffix));
    created_ = std::filesystem::create_directory(directory_, error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(created_);
  }
  void TearDown() override {
    if (created_) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
      EXPECT_FALSE(error);
    }
  }
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
    cached.code_cache = directory_;
    return compiler.Compile(prepared->snapshot().program, cached);
  }
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
  void SetDependency(std::string source) { dependency_ = std::move(source); }

 private:
  std::filesystem::path directory_;
  std::string dependency_ = "export func value(): int { return 40; }";
  bool created_ = false;
};

TEST_F(JitCodeCacheTest, RelocatesCachedObjectsIntoIndependentLiveBatchesAndFreshIsolates) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto first = Compile(**compiler, {.tier = NativeCodeTier::Optimized});
  ASSERT_TRUE(first);
  EXPECT_EQ(first->cache, CodeCacheStatus::Miss);
  const auto memory = (*compiler)->resource_counts();
  EXPECT_GT(memory.executable_bytes, 0U);
  EXPECT_GT(memory.read_only_bytes, 0U);
  EXPECT_GT(memory.debug_payload_bytes, 0U);
  {
    const auto second = Compile(**compiler, {.tier = NativeCodeTier::Optimized});
    ASSERT_TRUE(second);
    EXPECT_EQ(second->cache, CodeCacheStatus::Hit);
    EXPECT_EQ(second->timings.compilation.count(), 0);
    EXPECT_EQ(second->timings.optimization.count(), 0);
    EXPECT_NE(first->image.get(), second->image.get());
    EXPECT_EQ((*compiler)->resource_counts().batches, 2U);
  }
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
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

TEST_F(JitCodeCacheTest, CacheWriteFailureRetiresPreparedCodeAndLeavesTheRunningSessionUntouched) {
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

}  // namespace
}  // namespace vela::compiler::jit
