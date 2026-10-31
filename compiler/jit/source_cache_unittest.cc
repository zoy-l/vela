#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "compiler/frontend/diagnostic.h"
#include "gtest/gtest.h"

#include <memory>
#include <utility>
#include "base/file.h"
#include "base/path.h"
#include "base/testing/directory.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"

namespace vela::compiler {
namespace {

constexpr std::int64_t kAnswer = 42;
constexpr std::int64_t kChangedAnswer = 43;
constexpr std::int64_t kNextCount = 44;
constexpr double kDoubleAnswer = 42.0;

class NativeSourceCacheTest : public ::vela::test::DirectoryTest {
 protected:
  void SetUp() override {
    ::vela::test::DirectoryTest::SetUp();
    ASSERT_FALSE(HasFatalFailure());
    auto compiler = jit::Compiler::Create();
    ASSERT_TRUE(compiler);
    compiler_ = std::move(*compiler);
    Write("main.vela", "func main(): int { return 42; }");
  }
  void Write(const std::filesystem::path& name, std::string_view text) {
    const auto path = directory() / name;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(WriteFileAtomically(path, std::as_bytes(std::span(text))));
  }
  jit::SourceResult Compile(std::string_view entry = "main.vela",
                            frontend::DiagnosticOptions diagnostics = {}) {
    jit::SourceOptions options;
    options.diagnostics = diagnostics;
    options.compilation.code_cache = directory() / "cache";
    return jit::CompileFile(*compiler_, PathText(directory() / Utf8Path(entry)), options);
  }
  void ExpectRun(const jit::SourceResult& result, std::int64_t expected) {
    ASSERT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
    auto instance = runtime::NativeInstance::Instantiate(isolate_, result.code.image);
    ASSERT_TRUE(instance) << instance.error().message;
    ExpectEntry(**instance, expected);
  }
  static void ExpectEntry(runtime::NativeInstance& instance, std::int64_t expected) {
    const auto main = instance.FindFunction("main");
    ASSERT_TRUE(main);
    const auto executed = runtime::ExecuteNativeFunction(instance, *main);
    ASSERT_TRUE(executed.Succeeded()) << executed.message;
    EXPECT_EQ(executed.value.AsInteger(), expected);
  }
  runtime::Isolate& isolate() { return isolate_; }

 private:
  runtime::Isolate isolate_{1};
  std::unique_ptr<jit::Compiler> compiler_;
};

TEST_F(NativeSourceCacheTest, HitsInFreshIsolateWithoutPersistingRuntimeState) {
  Write("main.vela", "let count=40; func main(): int { count=count+2; return count; }");
  const auto first = Compile();
  ASSERT_TRUE(first.Succeeded()) << frontend::DiagnosticsText(first.diagnostics);
  EXPECT_EQ(first.code.cache, jit::CodeCacheStatus::Miss);
  auto instance = runtime::NativeInstance::Instantiate(isolate(), first.code.image);
  ASSERT_TRUE(instance);
  ExpectEntry(**instance, kAnswer);
  ExpectEntry(**instance, kNextCount);
  runtime::Isolate other(1);
  const auto second = Compile();
  ASSERT_TRUE(second.Succeeded()) << frontend::DiagnosticsText(second.diagnostics);
  EXPECT_EQ(second.code.cache, jit::CodeCacheStatus::Hit);
  auto loaded = runtime::NativeInstance::Instantiate(other, second.code.image);
  ASSERT_TRUE(loaded);
  ExpectEntry(**loaded, kAnswer);
}

TEST_F(NativeSourceCacheTest, MapLiteralDependenciesInvalidateAndRestoreConcreteMetadata) {
  Write("main.vela",
        "import 'library.vela'; func main(): int { return values().get('answer') ?? 0; }");
  Write("library.vela", "export func values(): Map<string,int> { return {'answer':42}; }");
  const auto first = Compile();
  ASSERT_TRUE(first.Succeeded()) << frontend::DiagnosticsText(first.diagnostics);
  ExpectRun(first, kAnswer);
  const auto cached = Compile();
  ASSERT_TRUE(cached.Succeeded()) << frontend::DiagnosticsText(cached.diagnostics);
  EXPECT_EQ(cached.code.cache, jit::CodeCacheStatus::Hit);
  ExpectRun(cached, kAnswer);
  Write("library.vela", "export func values(): Map<string,int> { return {'answer':43}; }");
  const auto changed = Compile();
  ASSERT_TRUE(changed.Succeeded()) << frontend::DiagnosticsText(changed.diagnostics);
  EXPECT_EQ(changed.code.cache, jit::CodeCacheStatus::Miss);
  ExpectRun(changed, kChangedAnswer);
}

TEST_F(NativeSourceCacheTest, InvalidatesChangedDependencyEvenWithSameTimestampAndSize) {
  Write("main.vela", "import 'lib'; func main(): int { return answer(); }");
  Write("lib.vela", "export func answer(): int { return 42; }");
  const auto first = Compile();
  ASSERT_TRUE(first.Succeeded()) << frontend::DiagnosticsText(first.diagnostics);
  ExpectRun(first, kAnswer);
  std::error_code error;
  const auto timestamp = std::filesystem::last_write_time(directory() / "lib.vela", error);
  ASSERT_FALSE(error);
  Write("lib.vela", "export func answer(): int { return 43; }");
  std::filesystem::last_write_time(directory() / "lib.vela", timestamp, error);
  ASSERT_FALSE(error);
  const auto second = Compile();
  ASSERT_TRUE(second.Succeeded()) << frontend::DiagnosticsText(second.diagnostics);
  EXPECT_EQ(second.code.cache, jit::CodeCacheStatus::Miss);
  ExpectRun(second, kChangedAnswer);
}

TEST_F(NativeSourceCacheTest, ValidatesEmbeddedSdkInputsWithoutReadingThemAsDiskPaths) {
  Write("main.vela", R"(import 'sdk:test'; func main(): int {
    return Timeout(42).milliseconds;
  })");
  const auto first = Compile();
  ASSERT_TRUE(first.Succeeded()) << frontend::DiagnosticsText(first.diagnostics);
  EXPECT_EQ(first.code.cache, jit::CodeCacheStatus::Miss);
  ExpectRun(first, kAnswer);
  const auto second = Compile();
  ASSERT_TRUE(second.Succeeded()) << frontend::DiagnosticsText(second.diagnostics);
  EXPECT_EQ(second.code.cache, jit::CodeCacheStatus::Hit);
  ExpectRun(second, kAnswer);
}

TEST_F(NativeSourceCacheTest, InvalidSourceCannotExecuteOrReplaceOldImage) {
  ASSERT_TRUE(Compile().Succeeded());
  Write("main.vela", "func main(): int { return true; }");
  const auto changed = Compile();
  EXPECT_FALSE(changed.Succeeded());
  EXPECT_FALSE(changed.diagnostics.empty());
  Write("main.vela", "func main(): int { return 42; }");
  const auto restored = Compile();
  ASSERT_TRUE(restored.Succeeded()) << frontend::DiagnosticsText(restored.diagnostics);
  EXPECT_EQ(restored.code.cache, jit::CodeCacheStatus::Hit);
}

TEST_F(NativeSourceCacheTest, SdkTestRegistrationCacheSurvivesCompilerLifetimes) {
  Write("main.vela", R"(import 'sdk:test';
func main(): void { test('small suite', () => { expect(42, equals(42)); }); }
)");
  {
    auto compiler = jit::Compiler::Create();
    ASSERT_TRUE(compiler);
    jit::SourceOptions options;
    options.compilation.code_cache = directory() / "cache";
    const auto initial = jit::CompileFile(**compiler, PathText(directory() / "main.vela"), options);
    ASSERT_TRUE(initial.Succeeded()) << frontend::DiagnosticsText(initial.diagnostics);
    EXPECT_EQ(initial.code.cache, jit::CodeCacheStatus::Miss);
  }
  const auto cached = Compile();
  ASSERT_TRUE(cached.Succeeded()) << frontend::DiagnosticsText(cached.diagnostics);
  EXPECT_EQ(cached.code.cache, jit::CodeCacheStatus::Hit);
}

TEST_F(NativeSourceCacheTest,
       GenericMethodImagesRestoreAndChangedBoundsInvalidateTheirDependencies) {
  Write("main.vela", R"(
import 'library.vela';
func main(): int { return Operations().read(Value()); }
)");
  const std::string declarations = R"(
export interface Reader<T> { read(): T; }
export class Value implements Reader<int> { read(): int { return 42; } }
export class Operations {
  read<T extends Reader<int>>(value: T): int { return value.read(); }
}
)";
  Write("library.vela", declarations);
  const auto first = Compile();
  ASSERT_TRUE(first.Succeeded()) << frontend::DiagnosticsText(first.diagnostics);
  EXPECT_EQ(first.code.cache, jit::CodeCacheStatus::Miss);
  ExpectRun(first, kAnswer);
  const auto restored = Compile();
  ASSERT_TRUE(restored.Succeeded()) << frontend::DiagnosticsText(restored.diagnostics);
  EXPECT_EQ(restored.code.cache, jit::CodeCacheStatus::Hit);
  ExpectRun(restored, kAnswer);
  Write("library.vela", R"(
export interface Reader<T> { read(): T; }
export class Value implements Reader<int> { read(): int { return 42; } }
export class Operations {
  read<T extends Reader<string>>(value: T): int { return 42; }
}
)");
  const auto rejected = Compile();
  EXPECT_FALSE(rejected.Succeeded());
  EXPECT_FALSE(rejected.diagnostics.empty());
  Write("library.vela", declarations);
  const auto previous = Compile();
  ASSERT_TRUE(previous.Succeeded()) << frontend::DiagnosticsText(previous.diagnostics);
  EXPECT_EQ(previous.code.cache, jit::CodeCacheStatus::Hit);
  ExpectRun(previous, kAnswer);
}

TEST_F(NativeSourceCacheTest, JsonAndDynamicDispatchRetainMetadataInFreshIsolateCacheHit) {
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
  ASSERT_TRUE(first.Succeeded()) << frontend::DiagnosticsText(first.diagnostics);
  EXPECT_EQ(first.code.cache, jit::CodeCacheStatus::Miss);
  ExpectRun(first, kAnswer);
  runtime::Isolate fresh(1);
  const auto cached = Compile();
  ASSERT_TRUE(cached.Succeeded()) << frontend::DiagnosticsText(cached.diagnostics);
  ASSERT_EQ(cached.code.cache, jit::CodeCacheStatus::Hit);
  auto instance = runtime::NativeInstance::Instantiate(fresh, cached.code.image);
  ASSERT_TRUE(instance);
  ExpectEntry(**instance, kAnswer);
}

TEST_F(NativeSourceCacheTest, RebuildsCorruptCacheAndReportsUnwritableCacheDirectory) {
  ASSERT_TRUE(Compile().Succeeded());
  std::error_code error;
  const std::filesystem::directory_iterator files(directory() / "cache", error);
  ASSERT_FALSE(error);
  ASSERT_NE(files, std::filesystem::directory_iterator());
  constexpr std::string_view kCorrupt = "corrupt";
  ASSERT_TRUE(WriteFileAtomically(files->path(), std::as_bytes(std::span(kCorrupt))));
  const auto rebuilt = Compile();
  ASSERT_TRUE(rebuilt.Succeeded()) << frontend::DiagnosticsText(rebuilt.diagnostics);
  EXPECT_EQ(rebuilt.code.cache, jit::CodeCacheStatus::Invalid);
  ExpectRun(rebuilt, kAnswer);
  Write("blocked", "a file is not a cache directory");
  auto compiler = jit::Compiler::Create();
  ASSERT_TRUE(compiler);
  jit::SourceOptions options;
  options.compilation.code_cache = directory() / "blocked";
  EXPECT_FALSE(
      jit::CompileFile(**compiler, PathText(directory() / "main.vela"), options).Succeeded());
}

std::int64_t NativeAnswer(runtime::NativeContext& /*unused*/) noexcept {
  return kAnswer;
}
double WrongAnswer(runtime::NativeContext& /*unused*/) noexcept {
  return kDoubleAnswer;
}

TEST_F(NativeSourceCacheTest, LinksCurrentNativeRegistryAndRejectsSignatureChanges) {
  Write("main.vela", "external func answer(): int; func main(): int { return answer(); }");
  ASSERT_TRUE(isolate().RegisterNative(runtime::NativeFunction::Bind<NativeAnswer>("answer")));
  const auto first = Compile();
  ASSERT_TRUE(first.Succeeded()) << frontend::DiagnosticsText(first.diagnostics);
  ExpectRun(first, kAnswer);
  runtime::Isolate other;
  ASSERT_TRUE(other.RegisterNative(runtime::NativeFunction::Bind<WrongAnswer>("answer")));
  const auto changed = Compile();
  ASSERT_TRUE(changed.Succeeded());
  EXPECT_EQ(changed.code.cache, jit::CodeCacheStatus::Hit);
  const auto rejected = runtime::NativeInstance::Instantiate(other, changed.code.image);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, runtime::NativeInstantiationStatus::ImportSignatureMismatch);
  const auto original = Compile();
  ASSERT_TRUE(original.Succeeded()) << frontend::DiagnosticsText(original.diagnostics);
  EXPECT_EQ(original.code.cache, jit::CodeCacheStatus::Hit);
}

TEST_F(NativeSourceCacheTest, StandardEnvironmentAndUnicodePathsSurviveCacheHit) {
  Write(Utf8Path("你好.vela"), "func main(): int { return Math.sqrt(1764.0).toInt(); }");
  const auto first = Compile("你好.vela");
  ASSERT_TRUE(first.Succeeded()) << frontend::DiagnosticsText(first.diagnostics);
  ExpectRun(first, kAnswer);
  const auto second = Compile("你好.vela");
  ASSERT_TRUE(second.Succeeded()) << frontend::DiagnosticsText(second.diagnostics);
  EXPECT_EQ(second.code.cache, jit::CodeCacheStatus::Hit);
  ExpectRun(second, kAnswer);
}

TEST_F(NativeSourceCacheTest, PreservesWarningsOnHitsAndRechecksChangedSeverity) {
  Write("main.vela", "func main(): int { const value: dynamic = 42; return value; }");
  const auto first = Compile();
  ASSERT_TRUE(first.Succeeded()) << frontend::DiagnosticsText(first.diagnostics);
  ASSERT_TRUE(first.Succeeded());
  ASSERT_EQ(first.diagnostics.size(), 1U);
  const auto hit = Compile();
  ASSERT_TRUE(hit.Succeeded()) << frontend::DiagnosticsText(hit.diagnostics);
  EXPECT_EQ(hit.code.cache, jit::CodeCacheStatus::Hit);
  ASSERT_EQ(hit.diagnostics.size(), 1U);
  const auto& original = first.diagnostics.front();
  const auto& restored = hit.diagnostics.front();
  EXPECT_EQ(restored.code, original.code);
  EXPECT_EQ(restored.severity, frontend::DiagnosticSeverity::Warning);
  EXPECT_EQ(restored.location, original.location);
  EXPECT_EQ(restored.source_path, original.source_path);
  EXPECT_EQ(restored.message, original.message);
  ExpectRun(hit, kAnswer);
  const auto strict =
      Compile("main.vela", {.implicit_dynamic_conversion = frontend::DiagnosticSeverity::Error});
  EXPECT_FALSE(strict.Succeeded());
  EXPECT_FALSE(strict.diagnostics.empty());
}

}  // namespace
}  // namespace vela::compiler
