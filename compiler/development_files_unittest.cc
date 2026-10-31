#include "compiler/development_files.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "base/file.h"
#include "base/path.h"
#include "base/testing/directory.h"
#include "compiler/development_session.h"
#include "compiler/source_session.h"

namespace vela::compiler {
namespace {

class DevelopmentFilesTest : public test::DirectoryTest {
 protected:
  void Write(std::string_view path, std::string_view source) {
    ASSERT_TRUE(WriteFileAtomically(directory_ / Utf8Path(path), std::as_bytes(std::span(source))));
  }
  [[nodiscard]] std::string Path(std::string_view path) const {
    return PathText(directory_ / Utf8Path(path));
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

}  // namespace
}  // namespace vela::compiler
