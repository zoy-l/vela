#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "base/testing/directory.h"
#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "runtime/isolate.h"
#include "runtime/string.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class ImportTest : public test::DirectoryTest {
 protected:
  void Write(const std::filesystem::path& filename, std::string_view source) {
    std::ofstream output(directory_ / filename, std::ios::binary);
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    ASSERT_TRUE(output.good());
  }

  frontend::CheckResult Check() { return frontend::CheckFile(EntryPath()); }
  CompileResult Compile() { return CompileFile(EntryPath(), isolate_); }
  std::string RunText() {
    const auto compiled = Compile();
    if (!compiled.Succeeded()) {
      ADD_FAILURE() << "module compilation failed";
      return {};
    }
    const auto main = compiled.program.FindFunction("main");
    if (!main.IsValid()) {
      ADD_FAILURE() << "missing main";
      return {};
    }
    const auto index = main.index();
    const auto result = vm::VirtualMachine(isolate_).Execute(compiled.program, index);
    if (!result.Succeeded() || !result.value.IsString()) {
      ADD_FAILURE() << "module execution failed";
      return {};
    }
    return std::string(result.value.AsString()->value());
  }

 private:
  [[nodiscard]] std::string EntryPath() const {
    const auto encoded = (directory_ / "main.vela").generic_u8string();
    return {encoded.begin(), encoded.end()};
  }

  runtime::Isolate isolate_{1};
};

TEST_F(ImportTest, ChecksAndRunsEscapedUnicodePathsAndStringApis) {
  Write(std::filesystem::path(u8"文本.vela"), R"(
export func decorate(text: string): string {
  const builder = String();
  builder.append("[${text}]");
  return builder.toString();
})");
  Write("main.vela", R"(
import "\u6587\u672C.vela";
import "文本.vela";
func main(): string { return decorate("世界"); }
)");
  const auto checked = Check();
  EXPECT_TRUE(checked.Succeeded());
  EXPECT_EQ(checked.function_count, 2U);
  EXPECT_EQ(RunText(), "[世界]");
}

TEST_F(ImportTest, LoadsDiamondDependenciesOnceInDependencyOrder) {
  Write("shared.vela", R"(export func shared(): string { return "s"; })");
  Write("left.vela",
        R"(import "shared.vela"; export func left(): string { return "l${shared()}"; })");
  Write("right.vela",
        R"(import "shared.vela"; export func right(): string { return "r${shared()}"; })");
  Write("main.vela", R"(import "left.vela"; import "right.vela";
    func main(): string { return [left(), right()].join(":"); })");
  const auto checked = Check();
  EXPECT_TRUE(checked.Succeeded());
  EXPECT_EQ(checked.function_count, 4U);
  EXPECT_EQ(RunText(), "ls:rs");
}

TEST_F(ImportTest, DiagnosesMissingImportsInCheckingAndCompilation) {
  Write("main.vela", R"(import "missing.vela"; func main(): string { return ""; })");
  EXPECT_FALSE(Check().Succeeded());
  EXPECT_FALSE(Compile().Succeeded());
}

TEST_F(ImportTest, LinksFunctionCyclesWithoutRepeatingModuleLoading) {
  Write("main.vela", R"(import "other.vela";
    export func finish(): string { return "cycle"; }
    func main(): string { return other(); })");
  Write("other.vela", R"(import "main.vela"; export func other(): string { return finish(); })");
  EXPECT_TRUE(Check().Succeeded());
  EXPECT_EQ(RunText(), "cycle");
}

TEST_F(ImportTest, ImportedInvalidEscapesStopBothPathsBeforeExecution) {
  Write("main.vela", R"(import "invalid.vela"; func main(): string { return invalid(); })");
  Write("invalid.vela", R"(func invalid(): string { return "\u{110000}"; })");
  const auto checked = Check();
  ASSERT_FALSE(checked.Succeeded());
  EXPECT_TRUE(checked.diagnostics.front().source_path.ends_with("invalid.vela"));
  EXPECT_FALSE(Compile().Succeeded());
}

TEST_F(ImportTest, ImportedTypeErrorsKeepTheOriginalFileAndLocation) {
  Write("main.vela", "import \"broken.vela\";\nfunc main(): int { return 1; }\n");
  Write("broken.vela", "func broken(): int {\n  return missing;\n}\n");
  const auto checked = Check();
  const auto compiled = Compile();
  ASSERT_FALSE(checked.Succeeded());
  ASSERT_FALSE(compiled.Succeeded());
  const auto& check_error = checked.diagnostics.front();
  const auto& compile_error = compiled.diagnostics.front();
  EXPECT_TRUE(check_error.source_path.ends_with("broken.vela"));
  EXPECT_EQ(check_error.location.line, 2U);
  EXPECT_EQ(check_error.location.column, 10U);
  EXPECT_EQ(compile_error.source_path, check_error.source_path);
  EXPECT_EQ(compile_error.location.line, check_error.location.line);
  EXPECT_EQ(compile_error.location.column, check_error.location.column);
}

TEST_F(ImportTest, MissingImportsPointToTheImportingStringLiteral) {
  Write("main.vela", "\nimport \"missing.vela\";\nfunc main(): int { return 1; }\n");
  const auto checked = Check();
  ASSERT_FALSE(checked.Succeeded());
  const auto& diagnostic = checked.diagnostics.front();
  EXPECT_TRUE(diagnostic.source_path.ends_with("main.vela"));
  EXPECT_EQ(diagnostic.location.line, 2U);
  EXPECT_EQ(diagnostic.location.column, 8U);
}

}  // namespace
}  // namespace vela::compiler
