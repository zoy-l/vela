#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/compiler.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/binary.h"
#include "base/path.h"
#include "base/testing/directory.h"
#include "compiler/development/development_files.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/modules/source_progress.h"
#include "compiler/ir/ir.h"
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

TEST_F(ExternalCompilerTest, RejectsInvalidWireEnumsAtTheirByteOffset) {
  BinaryWriter writer;
  writer.Write(std::string_view("main"));
  writer.Write(std::numeric_limits<std::uint8_t>::max());
  const auto decoded = DecodeSourceConfiguration(writer.bytes());
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), "byte 12: invalid binary enum");
}

TEST_F(ExternalCompilerTest, PreservesInvalidUtf8OffsetsThroughTheDecoder) {
  BinaryWriter writer;
  writer.Write(std::string_view("ok\xff", 3));
  const auto decoded = DecodeSourceConfiguration(writer.bytes());
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), "byte 10: binary text is not valid UTF-8");
}

TEST_F(ExternalCompilerTest, StructLayoutsSurviveTheCompilerProcessProtocol) {
  SourceSession source("main.vela");
  const auto prepared = test::PrepareText(source, R"(
struct Position { x: double; y: double; }
struct Body { position: Position?; label: string; }
export func copy(value: Body): Body { return value; }
)");
  ASSERT_TRUE(prepared);
  const auto encoded = EncodeProgram(*prepared->snapshot().program);
  const auto decoded = DecodeProgram(encoded);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(encoded, EncodeProgram(*decoded));
  const auto body = std::ranges::find(decoded->classes, "Body", &ir::Class::name);
  ASSERT_NE(body, decoded->classes.end());
  ASSERT_NE(body->value_layout, nullptr);
  EXPECT_EQ(body->kind, ir::Class::Kind::Struct);
  EXPECT_EQ(body->value_layout->offsets, (std::vector<std::uint32_t>{0, 3}));
  EXPECT_EQ(body->value_layout->slots.size(), 4U);
  const auto copy = std::ranges::find(decoded->functions, "copy", &ir::Function::name);
  ASSERT_NE(copy, decoded->functions.end());
  ASSERT_NE(copy->return_type.value_layout, nullptr);
  EXPECT_EQ(copy->return_type.value_layout, body->value_layout);
  const auto& shape = *copy->return_type.value_layout;
  ASSERT_EQ(shape.fields.size(), 2U);
  EXPECT_TRUE(shape.fields.front().nullable);
  ASSERT_NE(shape.fields.front().value_layout, nullptr);
  EXPECT_EQ(shape.fields.front().value_layout->slots.size(), 2U);
  EXPECT_EQ(copy->value_types.front().value_layout->slots.size(), 4U);
  EXPECT_EQ(copy->value_types.front().value_layout, body->value_layout);
  const auto position = std::ranges::find(decoded->classes, "Position", &ir::Class::name);
  ASSERT_NE(position, decoded->classes.end());
  EXPECT_EQ(shape.fields.front().value_layout, position->value_layout);

  auto independent = *decoded;
  const auto copy_index = static_cast<std::size_t>(copy - decoded->functions.begin());
  auto& independent_function = independent.functions[copy_index];
  independent_function.return_type.value_layout =
      std::make_shared<frontend::StructLayout>(*copy->return_type.value_layout);
  EXPECT_EQ(EncodeFunction(*copy), EncodeFunction(independent_function));
  EXPECT_EQ(EncodeCompilationEnvironment(*decoded), EncodeCompilationEnvironment(independent));
}

TEST_F(ExternalCompilerTest, BorrowedStructReceiversSurviveCopiesAndCompilerProcessProtocol) {
  SourceSession source("main.vela");
  const auto prepared = test::PrepareText(source, R"(
struct Counter { value: int; advance(): void { this.value++; } }
export func main(): int { let value = Counter(value: 41); value.advance(); return value.value; }
)");
  ASSERT_TRUE(prepared);
  const auto& original = *prepared->snapshot().program;
  const auto decoded = DecodeProgram(EncodeProgram(original));
  ASSERT_TRUE(decoded) << decoded.error();
  bool borrowed = false;
  for (std::size_t index = 0; index < original.functions.size(); ++index) {
    const auto& instructions = original.functions[index].instructions;
    for (std::size_t instruction = 0; instruction < instructions.size(); ++instruction) {
      const auto& value = instructions[instruction];
      if (value.struct_receiver == nullptr) {
        continue;
      }
      borrowed = true;
      const auto copy = value;
      EXPECT_EQ(copy.struct_receiver, value.struct_receiver);
      const auto& restored = decoded->functions[index].instructions[instruction].struct_receiver;
      ASSERT_NE(restored, nullptr);
      EXPECT_EQ(restored->kind, value.struct_receiver->kind);
      EXPECT_EQ(restored->receiver, value.struct_receiver->receiver);
      EXPECT_EQ(restored->type.value_layout->slots.size(),
                value.struct_receiver->type.value_layout->slots.size());
    }
  }
  ASSERT_TRUE(borrowed);
  auto compiler = Compiler::CreateExternal({.executable = Executable(), .cache = directory()});
  ASSERT_TRUE(compiler) << frontend::DiagnosticsText(compiler.error());
  auto code = (*compiler)->Compile(*decoded, {.optimization = NativeOptimization::Release});
  ASSERT_TRUE(code) << frontend::DiagnosticsText(code.error());
  runtime::Isolate isolate;
  ASSERT_TRUE(test::PublishImage(isolate, code->image));
  const auto main = isolate.native_dispatch().FindEntry("main.vela", "main");
  ASSERT_TRUE(main);
  EXPECT_TRUE(test::ReturnsInteger(*main, 42));
}

TEST_F(ExternalCompilerTest, RejectsCyclicAndForwardStructLayoutReferences) {
  ir::Program program;
  program.classes.emplace_back();
  auto& owner = program.classes.front();
  const auto absent = EncodeProgram(program);
  owner.value_layout = std::make_shared<frontend::StructLayout>();
  auto encoded = EncodeProgram(program);
  const auto present = std::ranges::mismatch(encoded, absent);
  ASSERT_NE(present.in1, encoded.end());
  // 第一个差异是布局的 presence 标志，后面紧跟其首次定义编号。
  const auto layout_offset =
      static_cast<std::size_t>(present.in1 - encoded.begin()) + sizeof(std::uint8_t);
  ASSERT_LE(layout_offset + sizeof(std::uint32_t), encoded.size());
  encoded[layout_offset] = std::byte{1};
  const auto forward = DecodeProgram(encoded);
  ASSERT_FALSE(forward);
  EXPECT_NE(forward.error().find("invalid struct layout reference"), std::string::npos);

  owner.value_layout->fields.push_back({.name = {}, .value_layout = owner.value_layout});
  encoded = EncodeProgram(program);
  owner.value_layout->fields.clear();
  const auto cyclic = DecodeProgram(encoded);
  ASSERT_FALSE(cyclic);
  EXPECT_NE(cyclic.error().find("cyclic struct layout reference"), std::string::npos);
  encoded.pop_back();
  EXPECT_FALSE(DecodeProgram(encoded));
}

TEST_F(ExternalCompilerTest, PredicateBranchesAndNeverCallsSurviveTheCompilerProtocol) {
  SourceSession source("main.vela");
  const auto prepared = test::PrepareText(source, R"(
func isMissing(value: int?): bool { return value == null; }
func fail(): never { throw Error(message: 'missing'); }
func read(value: int?): int {
  const missing = isMissing;
  if (missing(value)) fail();
  return value;
}
export func main(): int {
  try { return read(null); }
  catch (error: Error) { return read(42); }
}
)");
  ASSERT_TRUE(prepared);
  const auto decoded = DecodeProgram(EncodeProgram(*prepared->snapshot().program));
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_TRUE(std::ranges::any_of(decoded->layouts.signatures, [](const auto& signature) {
    return signature.narrowed_parameter && signature.false_narrowed_parameter;
  }));
  const auto original = std::ranges::find_if(decoded->functions, [](const auto& function) {
    return std::ranges::any_of(function.instructions, [](const auto& instruction) {
      return instruction.no_normal_return;
    });
  });
  ASSERT_NE(original, decoded->functions.end());
  auto changed = *original;
  const auto instruction = std::ranges::find_if(
      changed.instructions, [](const auto& item) { return item.no_normal_return; });
  instruction->no_normal_return = false;
  EXPECT_NE(EncodeFunction(*original), EncodeFunction(changed));
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto compiler = Compiler::CreateExternal({.executable = Executable(), .cache = directory()});
    ASSERT_TRUE(compiler) << frontend::DiagnosticsText(compiler.error());
    auto code = (*compiler)->Compile(*decoded, {.optimization = NativeOptimization::Release});
    ASSERT_TRUE(code) << frontend::DiagnosticsText(code.error());
    runtime::Isolate isolate;
    ASSERT_TRUE(test::PublishImage(isolate, code->image));
    const auto main = isolate.native_dispatch().FindEntry("main.vela", "main");
    ASSERT_TRUE(main);
    ASSERT_TRUE(test::ReturnsInteger(*main, 42));
  }
}

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

TEST_F(ExternalCompilerTest, SourceProgressRunsOnTheCallerAndDrainsBeforeTheResponse) {
  auto compiler =
      Compiler::CreateExternal({.executable = Executable(), .cache = directory() / "cache"});
  ASSERT_TRUE(compiler);
  auto created = (*compiler)->CreateSources(
      {.entry = "main.vela", .diagnostics = {}, .reflection = {}, .imports = {}});
  ASSERT_TRUE(created);
  auto& source = **created;
  SourceUpdate update;
  update.changes.push_back(
      {.path = "main.vela", .text = "export func answer(): int { return 42; }"});
  constexpr std::size_t kLibraries = 32;
  for (std::size_t index = 0; index < kLibraries; ++index) {
    auto path = "library" + std::to_string(index) + ".vela";
    update.added_roots.push_back(path);
    update.changes.push_back({.path = std::move(path), .text = ""});
  }
  const auto caller = std::this_thread::get_id();
  std::size_t completed = 0;
  std::size_t loading = 0;
  const auto prepared =
      source.Prepare(std::move(update), {}, [&](const frontend::SourceProgress& event) {
        EXPECT_EQ(std::this_thread::get_id(), caller);
        EXPECT_EQ(event.total, kLibraries + 1);
        if (event.kind == frontend::SourceProgressKind::File) {
          EXPECT_EQ(loading, 0U);
          EXPECT_EQ(event.completed, ++completed);
          EXPECT_FALSE(event.path.empty());
        } else {
          ++loading;
          EXPECT_EQ(completed, kLibraries + 1);
        }
      });
  ASSERT_TRUE(prepared) << frontend::DiagnosticsText(prepared.error().diagnostics);
  EXPECT_EQ(completed, kLibraries + 1);
  EXPECT_EQ(loading, 1U);
  ASSERT_TRUE(source.Commit(*prepared));
  const auto silent = source.Prepare(
      {.base_revision = source.snapshot()->revision, .changes = {}, .added_roots = {}});
  ASSERT_TRUE(silent) << frontend::DiagnosticsText(silent.error().diagnostics);
  EXPECT_EQ(completed, kLibraries + 1);
  EXPECT_EQ(loading, 1U);
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
