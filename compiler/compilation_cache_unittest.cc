#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "compiler/frontend/diagnostic.h"
#include "gtest/gtest.h"

#include "base/file.h"
#include "base/path.h"
#include "compiler/compilation_cache.h"
#include "runtime/isolate.h"
#include "runtime/native_context.h"
#include "runtime/native_function.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

constexpr std::int64_t kAnswer = 42;
constexpr std::int64_t kChangedAnswer = 43;
constexpr std::int64_t kNextCount = 44;
constexpr double kDoubleAnswer = 42.0;

class CompilationCacheTest : public testing::Test {
 protected:
  void SetUp() override {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    ASSERT_FALSE(error);
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = temporary / ("vela-cache-" + std::to_string(suffix));
    created_ = std::filesystem::create_directory(directory_, error);
    ASSERT_TRUE(created_);
    ASSERT_FALSE(error);
    Write("main.vela", "func main(): int { return 42; }");
  }
  void TearDown() override {
    if (created_) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
      EXPECT_FALSE(error);
    }
  }
  void Write(const std::filesystem::path& name, std::string_view text) {
    const auto path = directory_ / name;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(WriteFileAtomically(path, std::as_bytes(std::span(text))));
  }
  std::expected<CachedCompileResult, std::string> Compile(std::string_view entry = "main.vela") {
    return CompileFileCached(PathText(directory_ / Utf8Path(entry)), directory_ / "cache",
                             isolate_);
  }
  void ExpectRun(const CachedCompileResult& result, std::int64_t expected) {
    for (const auto& diagnostic : result.compiled.diagnostics) {
      EXPECT_NE(diagnostic.severity, frontend::DiagnosticSeverity::Error) << diagnostic.message;
    }
    ASSERT_TRUE(result.compiled.Succeeded());
    const auto executed =
        vm::VirtualMachine(isolate_).Execute(result.compiled.program.FindFunction("main"));
    ASSERT_TRUE(executed.Succeeded()) << executed.message;
    EXPECT_EQ(executed.value.AsInteger(), expected);
  }
  [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
  runtime::Isolate& isolate() { return isolate_; }

 private:
  std::filesystem::path directory_;
  runtime::Isolate isolate_{1};
  bool created_ = false;
};

TEST_F(CompilationCacheTest, HitsInFreshIsolateWithoutPersistingRuntimeState) {
  Write("main.vela", "let count=40; func main(): int { count=count+2; return count; }");
  const auto first = Compile();
  ASSERT_TRUE(first) << first.error();
  EXPECT_EQ(first->status, CacheStatus::Absent);
  ExpectRun(*first, kAnswer);
  ExpectRun(*first, kNextCount);
  runtime::Isolate other(1);
  const auto second =
      CompileFileCached(PathText(directory() / "main.vela"), directory() / "cache", other);
  ASSERT_TRUE(second) << second.error();
  EXPECT_EQ(second->status, CacheStatus::Hit);
  const auto result =
      vm::VirtualMachine(other).Execute(second->compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST_F(CompilationCacheTest, InvalidatesChangedDependencyEvenWithSameTimestampAndSize) {
  Write("main.vela", "import 'lib'; func main(): int { return answer(); }");
  Write("lib.vela", "export func answer(): int { return 42; }");
  const auto first = Compile();
  ASSERT_TRUE(first);
  ExpectRun(*first, kAnswer);
  std::error_code error;
  const auto timestamp = std::filesystem::last_write_time(directory() / "lib.vela", error);
  ASSERT_FALSE(error);
  Write("lib.vela", "export func answer(): int { return 43; }");
  std::filesystem::last_write_time(directory() / "lib.vela", timestamp, error);
  ASSERT_FALSE(error);
  const auto second = Compile();
  ASSERT_TRUE(second);
  EXPECT_EQ(second->status, CacheStatus::Stale);
  ExpectRun(*second, kChangedAnswer);
}

TEST_F(CompilationCacheTest, ValidatesEmbeddedSdkInputsWithoutReadingThemAsDiskPaths) {
  Write("main.vela", R"(import 'sdk:test'; func main(): int {
    return Timeout(42).milliseconds;
  })");
  const auto first = Compile();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->status, CacheStatus::Absent);
  ExpectRun(*first, kAnswer);
  const auto second = Compile();
  ASSERT_TRUE(second);
  EXPECT_EQ(second->status, CacheStatus::Hit);
  ExpectRun(*second, kAnswer);
}

TEST_F(CompilationCacheTest, InvalidSourceCannotExecuteOrReplaceOldImage) {
  ASSERT_TRUE(Compile());
  Write("main.vela", "func main(): int { return true; }");
  const auto changed = Compile();
  ASSERT_TRUE(changed);
  EXPECT_EQ(changed->status, CacheStatus::Stale);
  EXPECT_FALSE(changed->compiled.Succeeded());
  Write("main.vela", "func main(): int { return 42; }");
  const auto restored = Compile();
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->status, CacheStatus::Hit);
}

TEST_F(CompilationCacheTest, JsonAndDynamicDispatchRetainMetadataInFreshIsolateCacheHit) {
  Write("main.vela", R"(class Payload {
  toJson(): int[] { return [40, 2]; }
}
func main(): int {
  const root: dynamic = Json.parse(Json.stringify(Payload()));
  const get = root.slice;
  const values: dynamic[] = get();
  return values[0] + values[1];
})");
  const auto first = Compile();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->status, CacheStatus::Absent);
  ExpectRun(*first, kAnswer);
  runtime::Isolate fresh(1);
  const auto cached =
      CompileFileCached(PathText(directory() / "main.vela"), directory() / "cache", fresh);
  ASSERT_TRUE(cached);
  ASSERT_EQ(cached->status, CacheStatus::Hit);
  const auto result =
      vm::VirtualMachine(fresh).Execute(cached->compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), kAnswer);
}

TEST_F(CompilationCacheTest, RebuildsCorruptCacheAndReportsUnwritableCacheDirectory) {
  ASSERT_TRUE(Compile());
  std::error_code error;
  const std::filesystem::directory_iterator files(directory() / "cache", error);
  ASSERT_FALSE(error);
  ASSERT_NE(files, std::filesystem::directory_iterator());
  constexpr std::string_view kCorrupt = "corrupt";
  ASSERT_TRUE(WriteFileAtomically(files->path(), std::as_bytes(std::span(kCorrupt))));
  const auto rebuilt = Compile();
  ASSERT_TRUE(rebuilt);
  EXPECT_EQ(rebuilt->status, CacheStatus::Invalid);
  ExpectRun(*rebuilt, kAnswer);
  Write("blocked", "a file is not a cache directory");
  EXPECT_FALSE(
      CompileFileCached(PathText(directory() / "main.vela"), directory() / "blocked", isolate()));
}

std::int64_t NativeAnswer(runtime::NativeContext& /*unused*/) noexcept {
  return kAnswer;
}
double WrongAnswer(runtime::NativeContext& /*unused*/) noexcept {
  return kDoubleAnswer;
}

TEST_F(CompilationCacheTest, LinksCurrentNativeRegistryAndRejectsSignatureChanges) {
  Write("main.vela", "external func answer(): int; func main(): int { return answer(); }");
  ASSERT_TRUE(isolate().RegisterNative(runtime::NativeFunction::Bind<NativeAnswer>("answer")));
  const auto first = Compile();
  ASSERT_TRUE(first);
  ExpectRun(*first, kAnswer);
  runtime::Isolate other;
  ASSERT_TRUE(other.RegisterNative(runtime::NativeFunction::Bind<WrongAnswer>("answer")));
  const auto changed =
      CompileFileCached(PathText(directory() / "main.vela"), directory() / "cache", other);
  ASSERT_TRUE(changed);
  EXPECT_EQ(changed->status, CacheStatus::Invalid);
  EXPECT_FALSE(changed->compiled.Succeeded());
  const auto original = Compile();
  ASSERT_TRUE(original);
  EXPECT_EQ(original->status, CacheStatus::Hit);
}

TEST_F(CompilationCacheTest, StandardEnvironmentAndUnicodePathsSurviveCacheHit) {
  Write(Utf8Path("你好.vela"), "func main(): int { return Math.sqrt(1764.0).toInt(); }");
  const auto first = Compile("你好.vela");
  ASSERT_TRUE(first);
  ExpectRun(*first, kAnswer);
  const auto second = Compile("你好.vela");
  ASSERT_TRUE(second);
  EXPECT_EQ(second->status, CacheStatus::Hit);
  ExpectRun(*second, kAnswer);
}

TEST_F(CompilationCacheTest, PreservesWarningsOnHitsAndRechecksChangedSeverity) {
  Write("main.vela", "func main(): int { const value: dynamic = 42; return value; }");
  const auto first = Compile();
  ASSERT_TRUE(first);
  ASSERT_TRUE(first->compiled.Succeeded());
  ASSERT_EQ(first->compiled.diagnostics.size(), 1U);
  const auto hit = Compile();
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->status, CacheStatus::Hit);
  ASSERT_EQ(hit->compiled.diagnostics.size(), 1U);
  const auto& original = first->compiled.diagnostics.front();
  const auto& restored = hit->compiled.diagnostics.front();
  EXPECT_EQ(restored.code, original.code);
  EXPECT_EQ(restored.severity, frontend::DiagnosticSeverity::Warning);
  EXPECT_EQ(restored.location, original.location);
  EXPECT_EQ(restored.source_path, original.source_path);
  EXPECT_EQ(restored.message, original.message);
  ExpectRun(*hit, kAnswer);
  const auto strict =
      CompileFileCached(PathText(directory() / "main.vela"), directory() / "cache", isolate(),
                        {.implicit_dynamic_conversion = frontend::DiagnosticSeverity::Error});
  ASSERT_TRUE(strict);
  EXPECT_EQ(strict->status, CacheStatus::Stale);
  EXPECT_FALSE(strict->compiled.Succeeded());
}

}  // namespace
}  // namespace vela::compiler
