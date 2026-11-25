#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/compiler.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <semaphore>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "base/path.h"
#include "base/testing/directory.h"
#include "compiler/development/development_files.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/jit/compiler_protocol.h"
#include "compiler/jit/test_support.h"
#include "compiler/native/compile_options.h"
#include "compiler/test_support/development_test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_dispatch.h"

namespace vela::compiler::jit {
namespace {

class ExternalCompilerTest : public vela::test::DirectoryTest {
 protected:
  [[nodiscard]] static std::filesystem::path Executable() {
#ifdef _WIN32
    return std::filesystem::current_path() / "vela.exe";
#else
    return std::filesystem::current_path() / "vela";
#endif
  }
};

TEST_F(ExternalCompilerTest, CacheSurvivesProcessRestartAndReloadKeepsStateAndCapturedCode) {
  SourceSession source("main.vela");
  const auto first = test::PrepareText(source, R"(
let count: int = 40;
func add(value: int = 1): int { count += value; return count; }
export func answer(): int { return add() + 100; }
)");
  ASSERT_TRUE(first);
  const auto encoded = EncodeProgram(*first->snapshot().program);
  const auto decoded = DecodeProgram(encoded);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(encoded, EncodeProgram(*decoded));
  auto truncated = encoded;
  truncated.pop_back();
  EXPECT_FALSE(DecodeProgram(truncated));
  const CompileOptions options{
      .optimization = NativeOptimization::Baseline,
      .materialization = Materialization::OnDemand,
  };
  {
    auto compiler = Compiler::CreateExternal({.executable = Executable(), .cache = directory()});
    ASSERT_TRUE(compiler) << frontend::DiagnosticsText(compiler.error());
    auto code = (*compiler)->Compile(*decoded, options);
    ASSERT_TRUE(code) << frontend::DiagnosticsText(code.error());
    runtime::Isolate isolate;
    ASSERT_TRUE(test::PublishImage(isolate, code->image));
    auto answer = isolate.native_dispatch().FindEntry("main.vela", "answer");
    ASSERT_TRUE(answer);
    ASSERT_TRUE(test::ReturnsInteger(*answer, 141));
  }
  std::map<std::filesystem::path, std::filesystem::file_time_type> cached;
  for (const auto& file : std::filesystem::directory_iterator(directory())) {
    if (file.path().extension() == ".vlobj") {
      cached.emplace(file.path(), file.last_write_time());
    }
  }
  ASSERT_FALSE(cached.empty());
  auto compiler = Compiler::CreateExternal({.executable = Executable(), .cache = directory()});
  ASSERT_TRUE(compiler) << frontend::DiagnosticsText(compiler.error());
  CodeReuseCache reuse;
  auto first_code = (*compiler)->Compile(*first->snapshot().program, options, &reuse);
  ASSERT_TRUE(first_code) << frontend::DiagnosticsText(first_code.error());
  ASSERT_TRUE(source.Commit(*first));
  runtime::Isolate isolate;
  ASSERT_TRUE(test::PublishImage(isolate, first_code->image));
  auto answer = isolate.native_dispatch().FindEntry("main.vela", "answer");
  ASSERT_TRUE(answer);
  auto captured = answer->Capture();
  ASSERT_TRUE(captured);
  ASSERT_TRUE(test::ReturnsInteger(*answer, 141));
  for (const auto& [path, timestamp] : cached) {
    EXPECT_EQ(std::filesystem::last_write_time(path), timestamp);
  }
  const auto second = test::PrepareText(source, R"(
let count: int = -100;
func add(value: int = 2): int { count += value; return count; }
export func answer(): int { return add() + 200; }
)");
  ASSERT_TRUE(second);
  auto next = (*compiler)->Compile(*second->snapshot().program, options, &reuse);
  ASSERT_TRUE(next) << frontend::DiagnosticsText(next.error());
  ASSERT_TRUE(test::PublishImage(isolate, next->image, 1));
  compiler->reset();
  ASSERT_TRUE(test::ReturnsInteger(*answer, 243));
  const auto old = captured->Invoke({});
  ASSERT_TRUE(old);
  // 捕获入口保留自己的 +100 函数体，命名的 add 调用仍解析当前发布。
  EXPECT_EQ(old->AsInteger(), 145);
}

TEST_F(ExternalCompilerTest, ExternalFileAndSourceSessionsCommitReloadAndRejectInvalidEdits) {
  const auto entry = directory() / "main.vela";
  const auto write = [&](std::string_view text) {
    std::ofstream file(entry, std::ios::binary);
    file << text;
  };
  write(
      "let count = 40; func main(): void {} export func answer(): int { count += 1; return count; "
      "}");
  auto compiler =
      Compiler::CreateExternal({.executable = Executable(), .cache = directory() / "cache"});
  ASSERT_TRUE(compiler) << frontend::DiagnosticsText(compiler.error());
  auto files_created = (*compiler)->CreateFiles(PathText(entry), {});
  ASSERT_TRUE(files_created);
  auto& files = **files_created;
  ASSERT_TRUE(files.StartWatching());
  auto update = files.Capture({});
  ASSERT_TRUE(update);
  DevelopmentOptions options;
  options.compilation.optimization = NativeOptimization::Baseline;
  options.compilation.materialization = Materialization::Eager;
  auto created = DevelopmentSession::Create(files.entry(), {}, options, std::move(*compiler));
  ASSERT_TRUE(created) << frontend::DiagnosticsText(created.error());
  auto& session = **created;
  auto loaded =
      compiler::test::ApplyUpdate(session, std::move(*update), DevelopmentOperation::Load);
  ASSERT_EQ(loaded.status, DevelopmentStatus::Accepted) << loaded.message;
  EXPECT_FALSE(session.snapshot()->program);
  ASSERT_TRUE(files.Accept(session.snapshot()));
  auto value = session.Invoke(files.entry(), "answer");
  ASSERT_TRUE(value);
  EXPECT_EQ(value->AsInteger(), 41);
  write("export func answer(): int { return ;");
  auto invalid = files.Capture(*session.snapshot());
  // 文件图捕获可直接拒绝语法错误；当前发布仍可执行。
  if (invalid) {
    auto rejected = session.Prepare(std::move(*invalid), DevelopmentOperation::HotReload);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().status, DevelopmentStatus::SourceError);
  }
  write(
      "let count = 100; func main(): void {} export func answer(): int { count += 2; return count; "
      "}");
  update = files.Capture(*session.snapshot());
  ASSERT_TRUE(update);
  loaded =
      compiler::test::ApplyUpdate(session, std::move(*update), DevelopmentOperation::HotReload);
  ASSERT_EQ(loaded.status, DevelopmentStatus::Accepted)
      << frontend::DiagnosticsText(loaded.diagnostics);
  ASSERT_TRUE(files.Accept(session.snapshot()));
  value = session.Invoke(files.entry(), "answer");
  ASSERT_TRUE(value);
  EXPECT_EQ(value->AsInteger(), 43);
  update = files.Capture(*session.snapshot());
  ASSERT_TRUE(update);
  loaded = compiler::test::ApplyUpdate(session, std::move(*update), DevelopmentOperation::Reload);
  ASSERT_EQ(loaded.status, DevelopmentStatus::Accepted)
      << frontend::DiagnosticsText(loaded.diagnostics);
  ASSERT_TRUE(files.Accept(session.snapshot()));
  value = session.Invoke(files.entry(), "answer");
  ASSERT_TRUE(value);
  EXPECT_EQ(value->AsInteger(), 102);
}

TEST_F(ExternalCompilerTest, FileChangesNotifyTheHostWithoutPolling) {
  const auto entry = directory() / "main.vela";
  {
    std::ofstream file(entry);
    file << "export func main(): int { return 1; }";
  }
  std::counting_semaphore<> changed(0);
  auto compiler =
      Compiler::CreateExternal({.executable = Executable(), .cache = directory() / "cache"});
  ASSERT_TRUE(compiler) << frontend::DiagnosticsText(compiler.error());
  auto files_created = (*compiler)->CreateFiles(PathText(entry), {});
  ASSERT_TRUE(files_created);
  auto& files = **files_created;
  ASSERT_TRUE(files.StartWatching([&changed] { changed.release(); }));
  auto original = files.Capture({});
  ASSERT_TRUE(original);
  DevelopmentOptions options;
  options.compilation.optimization = NativeOptimization::Baseline;
  auto session = DevelopmentSession::Create(files.entry(), {}, options, std::move(*compiler));
  ASSERT_TRUE(session) << frontend::DiagnosticsText(session.error());
  const auto loaded =
      compiler::test::ApplyUpdate(**session, std::move(*original), DevelopmentOperation::Load);
  ASSERT_EQ(loaded.status, DevelopmentStatus::Accepted) << loaded.message;
  ASSERT_TRUE(files.Accept((*session)->snapshot()));
  {
    std::ofstream file(entry);
    file << "export func main(): int { return 42; }";
  }
  // 同目录中的缓存也会产生事件；只在通知到达后消费，忽略未修改源码的通知。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::optional<SourceUpdate> update;
  while (!update && changed.try_acquire_until(deadline)) {
    auto captured = files.Poll();
    ASSERT_TRUE(captured) << frontend::DiagnosticsText(captured.error());
    update = std::move(*captured);
  }
  ASSERT_TRUE(update);
  ASSERT_EQ(update->changes.size(), 1U);
  EXPECT_EQ(update->changes.front().text, "export func main(): int { return 42; }");
  auto prepared = (*session)->Prepare(std::move(*update), DevelopmentOperation::HotReload);
  ASSERT_TRUE(prepared) << prepared.error().message;
  files_created->reset();
  const auto submitted = (*session)->Submit(std::move(*prepared));
  ASSERT_EQ(submitted.status, DevelopmentStatus::WaitingForSafepoint);
  const auto published = (*session)->Pump();
  ASSERT_EQ(published.status, DevelopmentStatus::Accepted) << published.message;
  const auto answer = (*session)->RunEntry();
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer->AsInteger(), 42);
}

TEST_F(ExternalCompilerTest, SourceDeltasRetainUnchangedTextsAndApplyDeletions) {
  auto compiler =
      Compiler::CreateExternal({.executable = Executable(), .cache = directory() / "cache"});
  ASSERT_TRUE(compiler);
  auto created = (*compiler)->CreateSources(
      {.entry = "main.vela", .diagnostics = {}, .reflection = {}, .imports = {}});
  ASSERT_TRUE(created);
  auto& source = **created;
  auto first = source.Prepare({
      .changes =
          {
              {.path = "main.vela", .text = "export func answer(): int { return 1; }"},
              {.path = "library.vela", .text = "export func helper(): int { return 2; }"},
          },
      .added_roots = {"library.vela"},
  });
  ASSERT_TRUE(first) << frontend::DiagnosticsText(first.error().diagnostics);
  ASSERT_TRUE(source.Commit(*first));
  const auto original = source.snapshot();
  const auto library = original->sources.at("library.vela");
  auto edited = source.Prepare({
      .base_revision = original->revision,
      .changes = {{.path = "main.vela", .text = "export func answer(): int { return 3; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(edited) << frontend::DiagnosticsText(edited.error().diagnostics);
  EXPECT_EQ(edited->snapshot().sources.at("library.vela"), library);
  EXPECT_NE(edited->snapshot().sources.at("main.vela"), original->sources.at("main.vela"));
  ASSERT_TRUE(source.Commit(*edited));
  auto removed = source.Prepare({
      .base_revision = source.snapshot()->revision,
      .changes = {{.path = "library.vela", .text = std::nullopt}},
      .added_roots = {},
  });
  ASSERT_TRUE(removed) << frontend::DiagnosticsText(removed.error().diagnostics);
  EXPECT_FALSE(removed->snapshot().sources.contains("library.vela"));
  EXPECT_EQ(*original->sources.at("library.vela"), *library);
}

TEST_F(ExternalCompilerTest, EagerPublicationPreparesUnusedEntriesWithoutExecutingThem) {
  SourceSession source("main.vela");
  std::string text = R"(
let count: int = 40;
export func initial(): int { count += 1; return count; }
func otherBody(): int { count += 2; return count; }
export func otherPage(): int { return otherBody(); }
)";
  // 大量未调用入口同时就绪时，加载器不能递归物化整个别名表。
  constexpr std::size_t kUnvisitedPages = 512;
  for (std::size_t index = 0; index < kUnvisitedPages; ++index) {
    text += "export func page" + std::to_string(index) + "(): int { return 99; }\n";
  }
  const auto prepared = test::PrepareText(source, text);
  ASSERT_TRUE(prepared);
  auto compiler = Compiler::CreateExternal({.executable = Executable(), .cache = directory()});
  ASSERT_TRUE(compiler) << frontend::DiagnosticsText(compiler.error());
  auto code = (*compiler)->Compile(*prepared->snapshot().program,
                                   {.optimization = NativeOptimization::Baseline});
  ASSERT_TRUE(code) << frontend::DiagnosticsText(code.error());
  const auto ready = (*compiler)->resource_counts();
  EXPECT_EQ(ready.functions, code->image->functions.size);
  EXPECT_GT(ready.executable_bytes, 0U);
  runtime::Isolate isolate;
  ASSERT_TRUE(test::PublishImage(isolate, code->image));
  auto first = isolate.native_dispatch().FindEntry("main.vela", "initial");
  auto other = isolate.native_dispatch().FindEntry("main.vela", "otherPage");
  ASSERT_TRUE(first);
  ASSERT_TRUE(other);
  ASSERT_TRUE(test::ReturnsInteger(*first, 41));
  ASSERT_TRUE(test::ReturnsInteger(*other, 43));
  EXPECT_EQ((*compiler)->resource_counts().functions, ready.functions);
  EXPECT_EQ((*compiler)->resource_counts().executable_bytes, ready.executable_bytes);
}

TEST_F(ExternalCompilerTest, OptimizedMachineCodeKeepsAsyncObjectsAndResumesAfterCollection) {
  SourceSession source("main.vela");
  const auto prepared = test::PrepareText(source, R"(
class Box { value: int; text: string; }
export async func answer(): Future<int> {
  const box = Box(value: 40, text: 'a' + 'b');
  await sleep(1);
  return box.value + box.text.length;
}
)");
  ASSERT_TRUE(prepared);
  auto compiler = Compiler::CreateExternal({.executable = Executable(), .cache = directory()});
  ASSERT_TRUE(compiler) << frontend::DiagnosticsText(compiler.error());
  auto code = (*compiler)->Compile(
      *prepared->snapshot().program,
      {.optimization = NativeOptimization::Release, .materialization = Materialization::OnDemand});
  ASSERT_TRUE(code) << frontend::DiagnosticsText(code.error());
  runtime::Isolate isolate(1);
  ASSERT_TRUE(test::PublishImage(isolate, code->image));
  auto answer = isolate.native_dispatch().FindEntry("main.vela", "answer");
  ASSERT_TRUE(answer);
  compiler->reset();
  code->image.reset();
  const auto started = answer->Invoke({});
  ASSERT_TRUE(started);
  ASSERT_TRUE(test::CompletesWithInteger(isolate, *started, 42));
}

}  // namespace
}  // namespace vela::compiler::jit
