#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker_test_support.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/program_test_support.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

struct BasicLanguageProgram final {
  std::string_view source;
  std::int64_t expected;
};

class BasicLanguageExecutionTest : public testing::TestWithParam<BasicLanguageProgram> {};

TEST_P(BasicLanguageExecutionTest, ChecksCompilesAndExecutesWithGcPressure) {
  const auto& parameter = GetParam();
  SCOPED_TRACE(parameter.source);
  runtime::Isolate isolate{1};
  const auto result = test::RunInteger(parameter.source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, parameter.expected);
}

INSTANTIATE_TEST_SUITE_P(
    SwitchNullAndScopes,
    BasicLanguageExecutionTest,
    testing::Values(
        BasicLanguageProgram{R"(func main(): int {
          let result = 0;
          switch (1) { case 0: result = 100; break; case 1: result = result + 2; break;
                       case 2: result = result + 4; break; default: result = 99; }
          return result;
        })",
                             2},
        BasicLanguageProgram{R"(func main(): int {
          let result = 0;
          switch (9) { case 0: result = 100; break; default: result = result + 2; break;
                       case 2: result = result + 4; }
          return result;
        })",
                             2},
        BasicLanguageProgram{R"(func main(): int {
          let value = 0;
          for (let i = 0; i < 5; i = i + 1) {
            switch (i) { case 0: case 1: continue;
                         case 2: value = value + 2; break;
                         default: while (true) { value = value + 1; break; } }
            value = value + 10;
          }
          return value;
        })",
                             34},
        BasicLanguageProgram{R"(func choose(value: int): int {
          switch (value) { case 1: case 2: return 40; default: return 2; }
        } func main(): int { return choose(2) + choose(9); })",
                             42},
        BasicLanguageProgram{R"(enum First { a, b } enum Second { a, b }
        func main(): int {
          switch (Second.b) { case Second.a: return 0; case Second.b: return 42;
                             default: return 1; }
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int {
          const base = 20; const target = base + 1;
          switch (42) { case target * 2: return 42; default: return 0; }
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int {
          switch ("世界") { case "世" + "界": return 42; default: return 0; }
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int {
          switch (true) { case !true: return 0; default: return 42; }
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int { let value = 40;
          switch (3) {} switch (4) { case 1: value = 0; }
          return value + 2;
        })",
                             42},
        BasicLanguageProgram{R"(func read(value: int?): int {
          if (value == null) return 2;
          return value + 2;
        } func main(): int { return read(38) + read(null); })",
                             42},
        BasicLanguageProgram{R"(func read(value: int?): int {
          return value != null ? value + 2 : 2;
        } func main(): int { return read(38) + read(null); })",
                             42},
        BasicLanguageProgram{R"(func choose(flag: bool): int {
          let value: int? = null;
          const selected = flag ? (value = 40) : (value = 2);
          return value;
        } func main(): int { return choose(true) + choose(false); })",
                             42},
        BasicLanguageProgram{R"(func read(value: double?): int {
          if (value != null && value > 1.5) return 40;
          return 2;
        } func main(): int { return read(2.0) + read(null); })",
                             42},
        BasicLanguageProgram{R"(func read(value: bool?): int {
          if (!(value == null)) { if (value) return 40; }
          return 2;
        } func main(): int { return read(true) + read(null); })",
                             42},
        BasicLanguageProgram{R"(class Box { value: int?;
          read(): int { const value = this.value; if (value != null) return value; return 2; }
        } func main(): int { let box: Box? = Box(value: 40);
          if (box == null) return 0;
          return box.read() + Box(value: null).read();
        })",
                             42},
        BasicLanguageProgram{R"(func get(value: string?): int {
          if (null == value || value.length == 0) return 2;
          return value.length;
        } func main(): int { return get("四十") + get(null) + 38; })",
                             42},
        BasicLanguageProgram{R"(func main(): int {
          const values: int?[] = [null, 40];
          values[0] = 2;
          const a = values[0]; const b = values[1];
          if (a != null && b != null) return a + b;
          return 0;
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int {
          let values: int[]? = [40, 2];
          if (values == null) return 0;
          return values[0] + values[1];
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int {
          const matrix: int[]?[] = [null, [40, 2]];
          const row = matrix[1];
          if (row != null) return row[0] + row[1];
          return 0;
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int {
          const values: string?[] = [null, "a" + "b"];
          for (let i = 0; i < 100; i = i + 1) { const garbage = ["x" + "y"]; }
          const text = values[1];
          if (text != null) return text.length + 40;
          return 0;
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int {
          const values: String?[] = [null, String()];
          const builder = values[1];
          if (builder != null) { builder.append("ok"); return builder.toString().length + 40; }
          return 0;
        })",
                             42},
        BasicLanguageProgram{R"(class Box { value: string?[]; }
        func main(): int {
          const boxes: Box?[] = [null, Box(value: [null, "ab"])];
          for (let i = 0; i < 100; i = i + 1) { const garbage = Box(value: ["x" + "y"]); }
          const box = boxes[1];
          if (box != null) { const text = box.value[1];
            if (text != null) return text.length + 40;
          }
          return 0;
        })",
                             42},
        BasicLanguageProgram{R"(func empty(): string? { return null; }
        func main(): int { const a: string? = "x"; const b: string? = "" + "x";
          if (a == b && empty() == null && null == null) return 42; return 0;
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int { let x: int? = null; x = 40; return x + 2; })",
                             42},
        BasicLanguageProgram{R"(func main(): int { const value = 40;
          { const value = value + 1; switch (41) { case value: break; } }
          return value + 2;
        })",
                             42},
        BasicLanguageProgram{
            R"(func main(): int { const result = later(); { const later = 0; } return result; }
        func later(): int { return 42; })",
            42},
        BasicLanguageProgram{R"(func main(): int { let value = 40;
          const result = value + (value = 2); return result;
        })",
                             42},
        BasicLanguageProgram{R"(class Box { value: Box?; }
        func main(): int { let box: Box? = Box(value: null); const saved = box;
          if (box != null) { box.value = (box = null); }
          if (saved != null && saved.value == null && box == null) return 42;
          return 0;
        })",
                             42},
        BasicLanguageProgram{R"(func main(): int { let array: bool?[]? = [true, null];
          const saved = array;
          if (array != null) { array[1] = (array = null) == null; }
          if (saved != null) { const value = saved[1]; if (value != null && value) return 42; }
          return 0;
        })",
                             42}));

struct BasicLanguageError final {
  std::string_view source;
  std::string_view message;
};

class BasicLanguageErrorTest : public testing::TestWithParam<BasicLanguageError> {};

TEST_P(BasicLanguageErrorTest, RejectsInvalidProgramsWithLocatedDiagnostics) {
  const auto& parameter = GetParam();
  SCOPED_TRACE(parameter.source);
  const auto checked = frontend::CheckTestSource(parameter.source);
  EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [&](const auto& diagnostic) -> bool {
    return diagnostic.message.contains(parameter.message) && diagnostic.location.line > 0 &&
           diagnostic.location.column > 0;
  }));
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(parameter.source, isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    SyntaxTypesAndFlow,
    BasicLanguageErrorTest,
    testing::Values(
        BasicLanguageError{"func f(): void { switch 1 {} }", "expected '('"},
        BasicLanguageError{"func f(): void { switch (1) { case 1 print(1); } }", "expected ':'"},
        BasicLanguageError{"func f(): void { switch (1) { print(1); } }", "expected 'case'"},
        BasicLanguageError{"func f(): void { case 1: print(1); }", "expected expression"},
        BasicLanguageError{"func f(): void { switch (1) { case 1: break; case 1: break; } }",
                           "duplicate case"},
        BasicLanguageError{"func f(): void { switch (1) { case 1+1: break; case 2: break; } }",
                           "duplicate case"},
        BasicLanguageError{"func f(): void { switch (1) { default: break; default: break; } }",
                           "duplicate default"},
        BasicLanguageError{"func f(): void { switch (1) { case true: break; } }", "label type"},
        BasicLanguageError{"func f(): void { switch (1.0) {} }", "switch requires"},
        BasicLanguageError{"func f(x: int?): void { switch (x) {} }", "switch requires"},
        BasicLanguageError{"func f(x: int): void { switch (1) { case x: break; } }",
                           "compile-time constant"},
        BasicLanguageError{"func f(): int { switch (1) { case 1: return 1; } }", "fall through"},
        BasicLanguageError{"func f(): int { switch (1) { default: break; } }", "fall through"},
        BasicLanguageError{"func f(): void { switch (1) { default: continue; } }", "inside a loop"},
        BasicLanguageError{"func f(): void { switch (1) { default: break; print(1); } }",
                           "unreachable"},
        BasicLanguageError{"func f(): void { switch (1) { default: return; } print(1); }",
                           "unreachable"},
        BasicLanguageError{"func f(): int { return null; }", "cannot return"},
        BasicLanguageError{"func f(): int? { return 1.0; }", "cannot return"},
        BasicLanguageError{"func f(): void { let x: int = null; }", "cannot initialize"},
        BasicLanguageError{"func f(): void { let x = null; }", "explicit nullable type"},
        BasicLanguageError{"func f(): void { let x: int?? = null; }", "already nullable"},
        BasicLanguageError{"func f(): void { let x: void? = null; }", "void is not"},
        BasicLanguageError{"func f(x: int?): int { return x + 1; }", "arithmetic operands"},
        BasicLanguageError{"func f(x: bool?): void { if (x) {} }", "condition must be bool"},
        BasicLanguageError{"func f(x: string?): int { return x.length; }", "non-null check"},
        BasicLanguageError{"func f(x: string?): string { return x.toString(); }", "non-null check"},
        BasicLanguageError{"func f(x: int[]?): int { return x[0]; }", "non-null check"},
        BasicLanguageError{"func f(x: int?): void { const a: int[] = [x]; }", "array element type"},
        BasicLanguageError{"func f(x: int[]): void { const a: int?[] = x; }", "cannot initialize"},
        BasicLanguageError{"func f(x: int?, flag: bool): int { if (x != null) { if (flag) x = "
                           "null; return x+1; } return 0; }",
                           "arithmetic operands"},
        BasicLanguageError{"func f(x: int?, flag: bool): void { if (x != null) while (flag) { "
                           "print(x+1); x=null; } }",
                           "arithmetic operands"},
        BasicLanguageError{
            "func f(x: int?): int { if (x != null && (x = null) == null) return x+1; return 0; }",
            "arithmetic operands"},
        BasicLanguageError{"func f(x: int?): int { if (x != null) { switch (1) { default: x=null; "
                           "} return x+1; } return 0; }",
                           "arithmetic operands"},
        BasicLanguageError{"func f(x: int): int { let x = 1; return x; }", "duplicate local"},
        BasicLanguageError{"func f(): int { return later; let later = 1; }", "unknown name"},
        BasicLanguageError{"func f(flag: bool): int { if (flag) let x=1; return x; }",
                           "unknown name"},
        BasicLanguageError{"func f(): int { for (let i=0; i<1; i=i+1) {} return i; }",
                           "unknown name"},
        BasicLanguageError{"func g(): int { return 1; } func f(): int { const g=1; return g(); }",
                           "not callable"},
        BasicLanguageError{"func f(): void { const this = 1; }", "reserved"},
        BasicLanguageError{"class A { x: int; x(): int { return 1; } }", "already used"},
        BasicLanguageError{"class A {} func A(): int { return 1; }", "duplicate"},
        BasicLanguageError{"func v(): void {} func main(): bool { return v() == v(); }",
                           "value operands"},
        BasicLanguageError{"func v(): void {} func main(): void { print(v()); }",
                           "print requires a value"},
        BasicLanguageError{"func main(): void { const values = [null]; }",
                           "expected nullable type"}));

TEST(DiagnosticLocationTest, ReportsTheDuplicateEnumMemberLocation) {
  const auto checked = frontend::CheckTestSource("enum E {\n  a,\n  a,\n}\n");
  ASSERT_EQ(checked.diagnostics.size(), 1U);
  const auto& diagnostic = checked.diagnostics.front();
  EXPECT_EQ(diagnostic.location.line, 3U);
  EXPECT_EQ(diagnostic.location.column, 3U);
}

TEST(DiagnosticLocationTest, TracksSourcesThroughMultiSourceCompilation) {
  runtime::Isolate isolate;
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import \"broken.vela\"; func valid(): int { return 1; }"},
      {.path = "broken.vela", .text = "func invalid(): int {\n  return missing;\n}"},
  });
  const auto compiled = CompileModules(sources, "main.vela", isolate);
  ASSERT_FALSE(compiled.Succeeded());
  const auto& diagnostic = compiled.diagnostics.front();
  EXPECT_EQ(diagnostic.source_path, "broken.vela");
  EXPECT_EQ(diagnostic.location.line, 2U);
  EXPECT_EQ(diagnostic.location.column, 10U);
}

}  // namespace
}  // namespace vela::compiler
