#include "compiler/development/development_files.h"

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "base/file.h"
#include "base/path.h"
#include "base/testing/directory.h"
#include "compiler/development/development_repl.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/repl/repl_source.h"

namespace vela::compiler {
namespace {

class DevelopmentFilesTest : public test::DirectoryTest {
 protected:
  void Write(std::string_view path, std::string_view source) {
    ASSERT_TRUE(
        WriteFileAtomically(directory() / Utf8Path(path), std::as_bytes(std::span(source))));
  }
  [[nodiscard]] std::string Path(std::string_view path) const {
    return PathText(directory() / Utf8Path(path));
  }
  void MakeDirectory(std::string_view path) {
    std::error_code error;
    EXPECT_TRUE(std::filesystem::create_directory(directory() / Utf8Path(path), error));
    ASSERT_FALSE(error) << error.message();
  }
  static std::expected<SourceUpdate, frontend::Diagnostics> AwaitChange(DevelopmentFiles& files) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      constexpr auto kEventWait = std::chrono::milliseconds(10);
      auto polled = files.Poll(kEventWait);
      if (!polled) {
        return std::unexpected(std::move(polled.error()));
      }
      if (*polled) {
        return std::move(**polled);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::unexpected(frontend::Diagnostics{
        {.location = {}, .message = "file event did not arrive", .source_path = {}},
    });
  }
};

TEST_F(DevelopmentFilesTest, CapturedTextsStayImmutableWhenFilesChangeBeforeCompilation) {
  Write("main.vela",
        "import 'library.vela' as lib; export func read(): int { return lib.answer(); }");
  Write("library.vela", "export func answer(): int { return 42; }");
  DevelopmentFiles files(Path("main.vela"));
  const CompiledSourceSnapshot empty;
  auto captured = files.Capture(empty);
  ASSERT_TRUE(captured);
  auto created = DevelopmentSession::Create(files.entry());
  ASSERT_TRUE(created);
  auto& session = **created;
  Write("library.vela", "export func answer(): int { return 100; }");
  auto candidate = session.Prepare(std::move(*captured), DevelopmentOperation::Load);
  ASSERT_TRUE(candidate);
  ASSERT_EQ(session.Submit(std::move(*candidate)).status, DevelopmentStatus::WaitingForSafepoint);
  ASSERT_EQ(session.Pump().status, DevelopmentStatus::Accepted);
  const auto read = session.Invoke(files.entry(), "read");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->AsInteger(), 42);
  const auto next = files.Capture(*session.snapshot());
  ASSERT_TRUE(next);
  ASSERT_EQ(next->changes.size(), 1U);
  EXPECT_EQ(next->changes.front().path, Path("library.vela"));
}

TEST_F(DevelopmentFilesTest, DeletedImportedFilesCannotBeReadFromAnEarlierCaptureCache) {
  Write("main.vela", "import 'library.vela' as lib; func main(): int { return lib.answer(); }");
  Write("library.vela", "export func answer(): int { return 42; }");
  DevelopmentFiles files(Path("main.vela"));
  const CompiledSourceSnapshot empty;
  const auto first = files.Capture(empty);
  ASSERT_TRUE(first);
  SourceSession sources(files.entry());
  const auto candidate = sources.Prepare(*first);
  ASSERT_TRUE(candidate);
  ASSERT_TRUE(sources.Commit(*candidate));
  std::error_code error;
  EXPECT_TRUE(std::filesystem::remove(Utf8Path(Path("library.vela")), error));
  ASSERT_FALSE(error);
  const auto missing = files.Capture(*sources.snapshot());
  ASSERT_FALSE(missing);
  EXPECT_EQ(sources.snapshot()->revision, 1U);
}

TEST_F(DevelopmentFilesTest, EventCapturesKeepRejectedEditsAcrossOtherFileChanges) {
  Write("main.vela",
        "import 'library.vela' as lib; export func read(): int { return lib.answer(); }");
  Write("library.vela", "export func answer(): int { return 42; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  const CompiledSourceSnapshot empty;
  auto captured = files.Capture(empty);
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("library.vela", "export func answer(): int { return true; }");
  auto invalid = AwaitChange(files);
  ASSERT_TRUE(invalid);
  ASSERT_EQ(invalid->changes.size(), 1U);
  EXPECT_FALSE(sources.Prepare(std::move(*invalid)));
  Write("main.vela",
        "import 'library.vela' as lib; export func read(): int { return lib.answer() + 1; }");
  const auto combined = AwaitChange(files);
  ASSERT_TRUE(combined);
  ASSERT_EQ(combined->changes.size(), 2U);
  EXPECT_FALSE(sources.Prepare(*combined));
  Write("library.vela", "export func answer(): int { return 43; }");
  auto repaired = AwaitChange(files);
  ASSERT_TRUE(repaired);
  ASSERT_EQ(repaired->changes.size(), 2U);
  const auto candidate = sources.Prepare(std::move(*repaired));
  ASSERT_TRUE(candidate) << frontend::DiagnosticsText(candidate.error().diagnostics);
  ASSERT_TRUE(sources.Commit(*candidate));
}

TEST_F(DevelopmentFilesTest, ANewMissingImportIsWatchedUntilItsFileAppears) {
  Write("main.vela", "export func read(): int { return 42; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  const CompiledSourceSnapshot empty;
  auto captured = files.Capture(empty);
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("main.vela",
        "import 'library.vela' as lib; export func read(): int { return lib.answer(); }");
  EXPECT_FALSE(AwaitChange(files));
  Write("library.vela", "export func answer(): int { return 43; }");
  auto changed = AwaitChange(files);
  ASSERT_TRUE(changed) << frontend::DiagnosticsText(changed.error());
  ASSERT_EQ(changed->changes.size(), 2U);
  const auto candidate = sources.Prepare(std::move(*changed));
  ASSERT_TRUE(candidate) << frontend::DiagnosticsText(candidate.error().diagnostics);
}

TEST_F(DevelopmentFilesTest, ANewImportDirectoryIsWatchedAfterTheDirectoryAppears) {
  Write("main.vela", "export func read(): int { return 42; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  const CompiledSourceSnapshot empty;
  auto captured = files.Capture(empty);
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("main.vela",
        "import 'nested/library.vela' as lib; export func read(): int { return lib.answer(); }");
  EXPECT_FALSE(AwaitChange(files));
  MakeDirectory("nested");
  EXPECT_FALSE(AwaitChange(files));
  Write("nested/library.vela", "export func answer(): int { return 43; }");
  auto changed = AwaitChange(files);
  ASSERT_TRUE(changed) << frontend::DiagnosticsText(changed.error());
  const auto candidate = sources.Prepare(std::move(*changed));
  ASSERT_TRUE(candidate) << frontend::DiagnosticsText(candidate.error().diagnostics);
}

TEST_F(DevelopmentFilesTest, AncestorRenamesRebindNestedSourcesToTheReplacementDirectory) {
  MakeDirectory("library");
  MakeDirectory("library/nested");
  MakeDirectory("independent");
  Write("main.vela",
        "import 'library/nested/value.vela' as lib; "
        "import 'independent/value.vela' as other; "
        "export func read(): int { return lib.value() + other.value(); }");
  Write("library/nested/value.vela", "export func value(): int { return 40; }");
  Write("independent/value.vela", "export func value(): int { return 2; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  auto captured = files.Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));

  std::error_code error;
  std::filesystem::rename(Utf8Path(Path("library")), Utf8Path(Path("displaced")), error);
  ASSERT_FALSE(error) << error.message();
  auto missing = AwaitChange(files);
  if (missing) {
    EXPECT_FALSE(sources.Prepare(std::move(*missing)));
  } else {
    EXPECT_TRUE(frontend::HasErrors(missing.error()));
  }
  EXPECT_EQ(sources.snapshot()->revision, 1U);

  MakeDirectory("library");
  MakeDirectory("library/nested");
  Write("library/nested/value.vela", "export func value(): int { return 41; }");
  auto restored = AwaitChange(files);
  ASSERT_TRUE(restored) << frontend::DiagnosticsText(restored.error());
  const auto replacement = sources.Prepare(std::move(*restored));
  ASSERT_TRUE(replacement) << frontend::DiagnosticsText(replacement.error().diagnostics);
  ASSERT_TRUE(sources.Commit(*replacement));
  ASSERT_TRUE(files.Accept(sources.snapshot()));

  Write("library/nested/value.vela", "export func value(): int { return 42; }");
  auto changed = AwaitChange(files);
  ASSERT_TRUE(changed) << frontend::DiagnosticsText(changed.error());
  ASSERT_EQ(changed->changes.size(), 1U);
  EXPECT_EQ(changed->changes.front().path, Path("library/nested/value.vela"));
  const auto current = sources.Prepare(std::move(*changed));
  ASSERT_TRUE(current) << frontend::DiagnosticsText(current.error().diagnostics);
  ASSERT_TRUE(sources.Commit(*current));
  ASSERT_TRUE(files.Accept(sources.snapshot()));

  Write("independent/value.vela", "export func value(): int { return 3; }");
  const auto unrelated = AwaitChange(files);
  ASSERT_TRUE(unrelated) << frontend::DiagnosticsText(unrelated.error());
  ASSERT_EQ(unrelated->changes.size(), 1U);
  EXPECT_EQ(unrelated->changes.front().path, Path("independent/value.vela"));
}

TEST_F(DevelopmentFilesTest, ExplicitLoadsRegisterTheirAdditionalSourceDirectories) {
  Write("main.vela", "export func read(): int { return 42; }");
  MakeDirectory("nested");
  Write("nested/library.vela", "export func answer(): int { return 43; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  const CompiledSourceSnapshot empty;
  auto captured = files.Capture(empty);
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  auto loaded = DevelopmentFiles(Path("nested/library.vela")).Capture(*sources.snapshot());
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(files.WatchSources(loaded->changes));
  const auto additional = sources.Prepare(std::move(*loaded));
  ASSERT_TRUE(additional);
  ASSERT_TRUE(sources.Commit(*additional));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("nested/library.vela", "export func answer(): int { return 44; }");
  auto changed = AwaitChange(files);
  ASSERT_TRUE(changed);
  ASSERT_EQ(changed->changes.size(), 1U);
  EXPECT_EQ(changed->changes.front().path, Path("nested/library.vela"));
}

TEST_F(DevelopmentFilesTest, AcceptedReplImportsRegisterTheirSourceDirectories) {
  Write("main.vela", "export func read(): int { return 42; }");
  MakeDirectory("nested");
  Write("nested/library.vela", "export func answer(): int { return 43; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  auto captured = files.Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(captured);
  auto created = DevelopmentSession::Create(files.entry());
  ASSERT_TRUE(created);
  auto& session = **created;
  auto candidate = session.Prepare(std::move(*captured), DevelopmentOperation::Load);
  ASSERT_TRUE(candidate);
  static_cast<void>(session.Submit(std::move(*candidate)));
  ASSERT_EQ(session.Pump().status, DevelopmentStatus::Accepted);
  ASSERT_TRUE(files.Accept(session.snapshot()));
  DevelopmentRepl repl(session, Path("__repl__.vela"));
  const auto sdk = repl.Submit("import 'sdk:test' as testing;");
  ASSERT_EQ(sdk.status, SubmissionStatus::Executed) << frontend::DiagnosticsText(sdk.diagnostics);
  EXPECT_FALSE(session.snapshot()->sources.contains("sdk:test"));
  ASSERT_TRUE(files.Accept(session.snapshot()));
  const auto imported = repl.Submit("import 'nested/library.vela' as lib;");
  ASSERT_EQ(imported.status, SubmissionStatus::Executed)
      << frontend::DiagnosticsText(imported.diagnostics);
  ASSERT_TRUE(files.Accept(session.snapshot()));
  Write("nested/library.vela", "export func answer(): int { return 44; }");
  const auto changed = AwaitChange(files);
  ASSERT_TRUE(changed) << frontend::DiagnosticsText(changed.error());
  ASSERT_EQ(changed->changes.size(), 1U);
  EXPECT_EQ(changed->changes.front().path, Path("nested/library.vela"));
}

TEST_F(DevelopmentFilesTest, RemovedModulesAreNotReintroducedByLaterFileEvents) {
  Write("main.vela",
        "import 'nested/library.vela' as lib; export func read(): int { return lib.answer(); }");
  MakeDirectory("nested");
  Write("nested/library.vela", "export func answer(): int { return 43; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  auto captured = files.Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  const auto removed = sources.Prepare({
      .base_revision = sources.snapshot()->revision,
      .changes =
          {
              {.path = Path("main.vela"), .text = "export func read(): int { return 42; }"},
              {.path = Path("nested/library.vela"), .text = std::nullopt},
          },
      .added_roots = {},
  });
  ASSERT_TRUE(removed) << frontend::DiagnosticsText(removed.error().diagnostics);
  ASSERT_TRUE(sources.Commit(*removed));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("nested/library.vela", "export func answer(): int { return 44; }");
  const auto polled = files.Poll(std::chrono::milliseconds(100));
  ASSERT_TRUE(polled) << frontend::DiagnosticsText(polled.error());
  EXPECT_FALSE(*polled);
}

TEST_F(DevelopmentFilesTest, AcceptingAnOlderCaptureKeepsTheNewerPendingText) {
  Write("main.vela", "export func read(): int { return 42; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  auto captured = files.Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("main.vela", "export func read(): int { return 43; }");
  auto first = AwaitChange(files);
  ASSERT_TRUE(first);
  const auto candidate = sources.Prepare(std::move(*first));
  ASSERT_TRUE(candidate);
  Write("main.vela", "export func read(): int { return 44; }");
  const auto newer = AwaitChange(files);
  ASSERT_TRUE(newer);
  ASSERT_TRUE(sources.Commit(*candidate));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  const auto retained = files.Poll();
  ASSERT_TRUE(retained);
  ASSERT_TRUE(*retained);
  const auto& update = **retained;
  ASSERT_EQ(update.changes.size(), 1U);
  EXPECT_EQ(update.base_revision, sources.snapshot()->revision);
  EXPECT_EQ(update.changes.front().text, newer->changes.front().text);
}

TEST_F(DevelopmentFilesTest, SavingAcceptedContentsDoesNotPrepareAnUpdate) {
  constexpr std::string_view kSource = "export func read(): int { return 42; }";
  Write("main.vela", kSource);
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  auto captured = files.Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("main.vela", kSource);
  const auto unchanged = files.Poll(std::chrono::milliseconds(100));
  ASSERT_TRUE(unchanged) << frontend::DiagnosticsText(unchanged.error());
  EXPECT_FALSE(*unchanged);
}

TEST_F(DevelopmentFilesTest, RepeatedSavesOfRejectedContentsDoNotReemitPendingCandidates) {
  constexpr std::string_view kInvalid = "export func read(): int { return true; }";
  Write("main.vela", "export func read(): int { return 42; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  auto captured = files.Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("main.vela", kInvalid);
  const auto invalid = AwaitChange(files);
  ASSERT_TRUE(invalid);
  EXPECT_FALSE(sources.Prepare(*invalid));
  Write("main.vela", kInvalid);
  const auto duplicate = files.Poll(std::chrono::milliseconds(100));
  ASSERT_TRUE(duplicate) << frontend::DiagnosticsText(duplicate.error());
  EXPECT_FALSE(*duplicate);
  Write("main.vela", "export func read(): int { return 43; }");
  const auto repaired = AwaitChange(files);
  ASSERT_TRUE(repaired) << frontend::DiagnosticsText(repaired.error());
  ASSERT_EQ(repaired->changes.size(), 1U);
  EXPECT_EQ(repaired->changes.front().text, "export func read(): int { return 43; }");
  EXPECT_TRUE(sources.Prepare(*repaired));
}

TEST_F(DevelopmentFilesTest, AncestorImportsRemainIncrementalAfterSubscriptionUpgrades) {
  MakeDirectory("nested");
  Write("nested/main.vela",
        "import '../library.vela' as lib; export func read(): int { return lib.answer(); }");
  Write("library.vela", "export func answer(): int { return 42; }");
  DevelopmentFiles files(Path("nested/main.vela"));
  ASSERT_TRUE(files.StartWatching());
  auto captured = files.Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  const auto unchanged = files.Poll(std::chrono::milliseconds(100));
  ASSERT_TRUE(unchanged) << frontend::DiagnosticsText(unchanged.error());
  EXPECT_FALSE(*unchanged);
  const auto idle = files.Poll(std::chrono::milliseconds(100));
  ASSERT_TRUE(idle) << frontend::DiagnosticsText(idle.error());
  EXPECT_FALSE(*idle);
  Write("library.vela", "export func answer(): int { return 43; }");
  const auto changed = AwaitChange(files);
  ASSERT_TRUE(changed) << frontend::DiagnosticsText(changed.error());
  ASSERT_EQ(changed->changes.size(), 1U);
  EXPECT_EQ(changed->changes.front().path, Path("library.vela"));
}

TEST_F(DevelopmentFilesTest, SupersededRejectedImportsRetireTheirUnacceptedSources) {
  constexpr std::string_view kOriginal = "export func read(): int { return 42; }";
  Write("main.vela", kOriginal);
  MakeDirectory("nested");
  Write("nested/library.vela", "export func answer(): bool { return true; }");
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  auto captured = files.Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(captured);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("main.vela",
        "import 'nested/library.vela' as lib; export func read(): int { return lib.answer(); }");
  auto invalid = AwaitChange(files);
  ASSERT_TRUE(invalid);
  EXPECT_FALSE(sources.Prepare(std::move(*invalid)));
  Write("main.vela", kOriginal);
  const auto reverted = AwaitChange(files);
  ASSERT_TRUE(reverted);
  EXPECT_TRUE(reverted->changes.empty());
  Write("nested/library.vela", "export func answer(): int { return 43; }");
  const auto unrelated = files.Poll(std::chrono::milliseconds(100));
  ASSERT_TRUE(unrelated);
  EXPECT_FALSE(*unrelated);
}

TEST_F(DevelopmentFilesTest, SdkModulesRemainEmbeddedAcrossCaptureAndFileEvents) {
  constexpr std::string_view kSource =
      "import 'sdk:test' as testing; export func read(): int { return 42; }";
  Write("main.vela", kSource);
  DevelopmentFiles files(Path("main.vela"));
  ASSERT_TRUE(files.StartWatching());
  auto captured = files.Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(captured) << frontend::DiagnosticsText(captured.error());
  ASSERT_EQ(captured->changes.size(), 1U);
  SourceSession sources(files.entry());
  const auto initial = sources.Prepare(std::move(*captured));
  ASSERT_TRUE(initial) << frontend::DiagnosticsText(initial.error().diagnostics);
  ASSERT_TRUE(sources.Commit(*initial));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  EXPECT_FALSE(sources.snapshot()->sources.contains("sdk:test"));
  const auto unchanged = files.Capture(*sources.snapshot());
  ASSERT_TRUE(unchanged) << frontend::DiagnosticsText(unchanged.error());
  EXPECT_TRUE(unchanged->changes.empty());
  Write("main.vela", "import 'sdk:test' as testing; export func read(): int { return 43; }");
  auto edited = AwaitChange(files);
  ASSERT_TRUE(edited) << frontend::DiagnosticsText(edited.error());
  ASSERT_EQ(edited->changes.size(), 1U);
  const auto candidate = sources.Prepare(std::move(*edited));
  ASSERT_TRUE(candidate) << frontend::DiagnosticsText(candidate.error().diagnostics);
  ASSERT_TRUE(sources.Commit(*candidate));
  ASSERT_TRUE(files.Accept(sources.snapshot()));
  Write("main.vela", "export func read(): int { return 44; }");
  auto removed_import = AwaitChange(files);
  ASSERT_TRUE(removed_import) << frontend::DiagnosticsText(removed_import.error());
  const auto retained = sources.Prepare(std::move(*removed_import));
  ASSERT_TRUE(retained) << frontend::DiagnosticsText(retained.error().diagnostics);
  EXPECT_TRUE(retained->snapshot().modules.sources.contains("sdk:test"));
}

}  // namespace
}  // namespace vela::compiler
