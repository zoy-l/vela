#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "vm/bytecode_image.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class BytecodeImageExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(BytecodeImageExecutionTest, RestoresOwnedProgramInFreshIsolate) {
  std::vector<std::byte> image;
  {
    runtime::Isolate source_isolate(1);
    const auto compiled = CompileSource(GetParam(), source_isolate);
    for (const auto& diagnostic : compiled.diagnostics) {
      ADD_FAILURE() << diagnostic.message;
    }
    ASSERT_TRUE(compiled.Succeeded());
    const auto saved = vm::SaveBytecode(compiled.program, source_isolate);
    ASSERT_TRUE(saved) << saved.error().message;
    image = *saved;
  }
  runtime::Isolate isolate(1);
  auto program = vm::LoadBytecode(image, isolate);
  ASSERT_TRUE(program) << program.error().offset << ' ' << program.error().message;
  std::ranges::fill(image, std::byte{0});
  isolate.heap().Collect();
  const auto result = vm::VirtualMachine(isolate).Execute(program->FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(
    ProductionPaths,
    BytecodeImageExecutionTest,
    testing::Values(
        "func main(): int { return 42; }",
        "func main(): int { return (4000000000 - 3999999958); }",
        "func main(): int { if (1.5 * 2.0 == 3.0) { return 42; } return 0; }",
        "func main(): int { const values: string[] = [\"你好\", \"hello\"]; "
        "if (join(values, \" \") == \"你好 hello\") { return 42; } return 0; }",
        "class Box { param value: int; } func main(): int { return Box(value: 42).value; }",
        "func main(): int { const values: int?[][] = [[null], [42]]; "
        "const value=values[1][0]; if(value != null) { return value; } return 0; }",
        "func main(): int { let value = 40; const next: () => int = (): int => { value=value+1; "
        "return value; }; "
        "next(); return next(); }",
        "class Box<T> { param value: T; } func main(): int { return Box<int>(value: 42).value; }",
        "interface Readable { read(): int; } class Box implements Readable { read(): int { return "
        "42; } } "
        "func main(): int { const item: Readable = Box(); return item.read(); }",
        "func main(): int { const result: Expected<int, string> = ok(42); return result.value(); }",
        "func main(): int { const values: Map<string, Set<int>> = Map<string, Set<int>>(); "
        "const set: Set<int> = Set<int>(); set.add(42); values.set(\"key\", set); "
        "const found = values.get(\"key\"); if (found != null) { return found.values()[0]; } "
        "return 0; }",
        "import \"std:math\" as math; func main(): int { return math.sqrt(1764.0).toInt(); }",
        "import \"std:error\"; func main(): int { try { let zero=0; return 1/zero; } "
        "catch (error: Error) { if (error.message == \"division by zero\") { return 42; } return "
        "0; } }",
        "class Failure { param value: int; } func main(): int { "
        "try { try { throw Failure(value: 42); } finally { const text = \"allocated\" + \" text\"; "
        "} } "
        "catch (error: Failure) { return error.value; } }",
        "let value = 40; func main(): int { value=value+2; return value; }"));

TEST(BytecodeImageTest, RejectsTruncationWithoutExecutingBytes) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("func main(): int { return 42; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto saved = vm::SaveBytecode(compiled.program, isolate);
  ASSERT_TRUE(saved);
  runtime::Isolate other;
  for (std::size_t length = 0; length < saved->size(); ++length) {
    EXPECT_FALSE(vm::LoadBytecode(std::span(*saved).first(length), other));
  }
}

TEST(BytecodeImageTest, RejectsWrongIsolateAndChecksum) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("func main(): int { return 42; }", isolate);
  const auto saved = vm::SaveBytecode(compiled.program, isolate);
  ASSERT_TRUE(saved);
  runtime::Isolate other;
  EXPECT_FALSE(vm::SaveBytecode(compiled.program, other));
  auto changed = *saved;
  changed.back() ^= std::byte{1};
  const auto invalid = vm::LoadBytecode(changed, other);
  ASSERT_FALSE(invalid);
  EXPECT_TRUE(invalid.error().message.contains("checksum"));
}

TEST(BytecodeImageTest, ReloadsFreshGlobalStateAndRetainsSourceTrace) {
  runtime::Isolate isolate;
  const auto compiled =
      CompileSource("let value = 40; func main(): int { value=value+2; return value; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  vm::VirtualMachine machine(isolate);
  EXPECT_EQ(machine.Execute(compiled.program.FindFunction("main")).value.AsInteger(), 42);
  const auto saved = vm::SaveBytecode(compiled.program, isolate);
  ASSERT_TRUE(saved);
  const auto loaded = vm::LoadBytecode(*saved, isolate);
  ASSERT_TRUE(loaded);
  EXPECT_EQ(machine.Execute(loaded->FindFunction("main")).value.AsInteger(), 42);
  EXPECT_EQ(machine.Execute(compiled.program.FindFunction("main")).value.AsInteger(), 44);
}

TEST(BytecodeImageTest, EncodingFailureDoesNotRetainNewLayoutMetadata) {
  runtime::Isolate isolate;
  const auto initial = isolate.layout_counts();
  constexpr std::uint32_t kFieldCount =
      static_cast<std::uint32_t>(std::numeric_limits<std::uint8_t>::max()) + 2;
  std::string source = "class Large {";
  for (std::uint32_t index = 0; index < kFieldCount; ++index) {
    source += "value" + std::to_string(index) + ": int = 0;";
  }
  source += "} func main(): int { const value = Large(); return 0; }";
  const auto compiled = CompileSource(source, isolate);
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_EQ(isolate.layout_counts(), initial);
}

}  // namespace
}  // namespace vela::compiler
