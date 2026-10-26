#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "base/execution_status.h"
#include "compiler/compiler.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/bytecode_image.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class RegExpProgramTest : public testing::TestWithParam<std::string_view> {};

void ExpectFutureAnswer(runtime::Future& result, runtime::Isolate& isolate) {
  ASSERT_TRUE(isolate.scheduler().PumpUntil(result));
  ASSERT_EQ(result.state(), runtime::FutureState::Succeeded);
  const auto value = result.value().value_or(runtime::Value::Null());
  ASSERT_TRUE(value.IsInteger());
  EXPECT_EQ(value.AsInteger(), 42);
}

void ExpectAnswer(const vm::Program& program, runtime::Isolate& isolate) {
  const auto main = program.FindFunction("main");
  if (main.is_async()) {
    auto result = vm::VirtualMachine(isolate).ExecuteFuture(program, main.index());
    ExpectFutureAnswer(result, isolate);
    return;
  }
  const auto result = vm::VirtualMachine(isolate).Execute(main);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST_P(RegExpProgramTest, ExecutesWithGCAndReleasesProgramsAndWorkBuffers) {
  runtime::Isolate isolate(1);
  const auto baseline = isolate.heap().object_count();
  {
    const auto compiled = CompileSource(GetParam(), isolate);
    for (const auto& diagnostic : compiled.diagnostics) {
      ADD_FAILURE() << diagnostic.location.line << ':' << diagnostic.message;
    }
    ASSERT_TRUE(compiled.Succeeded());
    ExpectAnswer(compiled.program, isolate);
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

TEST_P(RegExpProgramTest, RestoresSourceFreeImageInFreshIsolate) {
  std::vector<std::byte> image;
  {
    runtime::Isolate isolate(1);
    const auto compiled = CompileSource(GetParam(), isolate);
    ASSERT_TRUE(compiled.Succeeded());
    const auto saved = vm::SaveBytecode(compiled.program, isolate);
    ASSERT_TRUE(saved) << saved.error().message;
    image = *saved;
  }
  runtime::Isolate isolate(1);
  const auto loaded = vm::LoadBytecode(image, isolate);
  ASSERT_TRUE(loaded) << loaded.error().message;
  std::ranges::fill(image, std::byte{0});
  isolate.heap().Collect();
  ExpectAnswer(*loaded, isolate);
}

INSTANTIATE_TEST_SUITE_P(ProductionContracts,
                         RegExpProgramTest,
                         testing::Values(
                             R"(
func main(): int {
  const re = RegExp('a.b');
  return re.pattern == 'a.b' && !re.isMultiLine && re.isCaseSensitive && !re.isDotAll &&
    re.isUnicode && !re.hasMatch('Axb') && !re.hasMatch('a\nb') &&
    RegExp('a.b', dotAll: true, caseSensitive: false).hasMatch('A\nb') ? 42 : 0;
})",
                             R"(
func main(): int {
  const re = RegExp('(?<word>\\w+)-(\\d+)(x)?()');
  const m = re.firstMatch('😀语言-٤٢');
  if (m == null) { return 0; }
  const groups: string?[] = m.groups([0, 1, 2, 3, 4]);
  return m.start == 1 && m.end == 6 && m.input == '😀语言-٤٢' && m.pattern == re &&
    m.groupCount == 4 && m[1] == '语言' && m.namedGroup('word') == '语言' &&
    groups[2] == '٤٢' && groups[3] == null && groups[4] == '' &&
    m.groupNames[0] == 'word' ? 42 : 0;
})",
                             R"(
func main(): int {
  const empty = RegExp('').allMatches('😀中');
  const alternatives = RegExp('a|ab').stringMatch('ab');
  return empty.length == 3 && empty[0].start == 0 && empty[1].start == 1 &&
    empty[2].start == 2 && empty[2].end == 2 && alternatives == 'a' &&
    RegExp('a+?').stringMatch('aaa') == 'a' && RegExp('a+').stringMatch('aaa') == 'aaa' ? 42 : 0;
})",
                             R"(
func main(): int {
  const re = RegExp('(?<=😀)(中)(?=文)');
  const m = re.matchAsPrefix('😀中文', start: 1);
  return m?.start == 1 && m?.end == 2 && re.matchAsPrefix('😀中文') == null &&
    RegExp('中').allMatches('中😀中', 1)[0].start == 2 &&
    RegExp('x').firstMatch('a') == null && RegExp('x').stringMatch('a') == null ? 42 : 0;
})",
                             R"(
func main(): int {
  const literal = '.*[](){}?+^$\\ #\n😀\0';
  const re = RegExp('(?x)^' + RegExp.escape(literal) + '$');
  return re.hasMatch(literal) && RegExp('a\0b').hasMatch('xa\0by') &&
    RegExp('(?i)é').hasMatch('É') && RegExp('\\p{Greek}+').hasMatch('Σλ') ? 42 : 0;
})",
                             R"(
func main(): int {
  return RegExp('^b$', multiLine: true).hasMatch('a\r\nb\r\nc') &&
    RegExp('^b$', multiLine: true).hasMatch('a\u2028b\u2029c') &&
    RegExp('a\\Rb').hasMatch('a\u0085b') &&
    !RegExp('(*LF)^b$', multiLine: true).hasMatch('a\rb\rc') ? 42 : 0;
})",
                             R"(
func main(): int {
  const re = RegExp('(?J)(?<x>a)|(?<x>b)');
  const a = re.firstMatch('a');
  const b = re.firstMatch('b');
  if (a == null || b == null) { return 0; }
  const names = a.groupNames;
  names[0] = 'changed';
  return a.namedGroup('x') == 'a' && b.namedGroup('x') == 'b' &&
    a.groupNames.length == 1 && a.groupNames[0] == 'x' &&
    RegExp('(?|(a)|(b))\\1').hasMatch('bb') &&
    RegExp('(?<=a{1,3})b').hasMatch('aaab') ? 42 : 0;
})",
                             R"(
func main(): int {
  const prefix = RegExp('a\\Kb').matchAsPrefix('ab');
  const before = RegExp('(?<=(a))b').firstMatch('ab');
  const after = RegExp('a(?=(b))').firstMatch('ab');
  return prefix?.start == 1 && prefix?.end == 2 && before?.group(1) == 'a' &&
    after?.group(1) == 'b' && RegExp('(a)\\1').hasMatch('aa') ? 42 : 0;
})",
                             R"(
type Pattern = RegExp;
type Match = RegExpMatch;
class Box { public param re: Pattern; public param found: Match?; }
async func lookup(re: RegExp): Future<RegExpMatch?> { return re.firstMatch('42'); }
async func main(): Future<int> {
  const re = Pattern('(?<digits>\\d+)');
  const box = Box(re: re, found: await lookup(re));
  const m = box.found;
  if (m == null) { return 0; }
  const group: (int) => string? = m.group;
  const all: (string) => RegExpMatch[] = box.re.allMatches;
  const named = m.namedGroup;
  const capture: () => RegExp = (): RegExp => box.re;
  const set = Set<RegExp>(); set.add(re);
  const map = Map<string, RegExpMatch>(); map.set('m', m);
  return group(1) == '42' && named('digits') == '42' && all('42 7').length == 2 &&
    capture() == re && set.has(re) && map.get('m') == m ? 42 : 0;
})",
                             R"(
let order = 0;
func mark(n: int): bool { order = order * 10 + n; return n == 2; }
func main(): int {
  const re = RegExp('a', dotAll: mark(3), caseSensitive: mark(2), multiLine: mark(1));
  const maybe: RegExp? = null;
  const m: RegExpMatch? = null;
  return order == 321 && maybe?.allMatches('a') == null && m?.groupNames == null &&
    m?.[0] == null && re.hasMatch('a') && re.hasMatch('a') ? 42 : 0;
})",
                             R"(
func main(): int {
  let count = 0;
  try { RegExp('中('); } catch (e: Error) {
    if (e.code == 'RegExpSyntaxError' && e.message.includes('offset 2')) { count += 1; }
  } finally { count += 1; }
  try { RegExp('\\C'); } catch (e: Error) { if (e.code == 'RegExpSyntaxError') { count += 1; } }
  try { RegExp('(?=a\\K)'); } catch (e: Error) { if (e.code == 'RegExpSyntaxError') { count += 1; } }
  try { RegExp('(?<=a*)b'); } catch (e: Error) { if (e.code == 'RegExpSyntaxError') { count += 1; } }
  const re = RegExp('(a)'); const m = re.firstMatch('a');
  if (m == null) { return 0; }
  try { m.group(-1); } catch (e: Error) { if (e.code == 'IndexOutOfBounds') { count += 1; } }
  try { m?.groups([0, 2]); } catch (e: Error) { if (e.code == 'IndexOutOfBounds') { count += 1; } }
  try { m?.namedGroup('missing'); } catch (e: Error) { if (e.code == 'RegExpGroupError') { count += 1; } }
  try { re.allMatches('a', -1); } catch (e: Error) { if (e.code == 'IndexOutOfBounds') { count += 1; } }
  try { re.matchAsPrefix('a', 2); } catch (e: Error) { if (e.code == 'IndexOutOfBounds') { count += 1; } }
  return count == 10 && re.hasMatch('a') ? 42 : 0;
})",
                             R"(
func main(): int {
  const found = RegExp('(?<a>中+)').firstMatch('😀中中');
  if (found == null) { return 0; }
  let total = 0;
  for (let i = 0; i < 150; i += 1) {
    total += RegExp('').allMatches('😀中中').length;
    if (found.namedGroup('a') != '中中') { return 0; }
  }
  return total == 600 && found.pattern.pattern == '(?<a>中+)' ? 42 : 0;
})",
                             R"(
func main(): int {
  const patterns = ['(*NO_START_OPT)(*LIMIT_MATCH=1)(a+)+$',
    '(*NO_START_OPT)(*LIMIT_DEPTH=1)(a+)+$', '(*NO_START_OPT)(*LIMIT_HEAP=0)(a+)+$'];
  let failures = 0;
  for (const source in patterns) {
    try { RegExp(source).hasMatch('aaaaaaaaaaaaaaaaaaaa!'); }
    catch (e: Error) { if (e.code == 'RegExpLimitError') { failures += 1; } }
    finally { failures += 1; }
  }
  try { RegExp('a|(?R)').allMatches('a!'); }
  catch (e: Error) { if (e.code == 'RegExpExecutionError') { failures += 1; } }
  return failures == 7 && RegExp('a').hasMatch('a') ? 42 : 0;
})",
                             R"(
func main(): int {
  const re = RegExp('x');
  const found = re.firstMatch('x');
  if (found == null) { return 0; }
  const copy: RegExpMatch = found as RegExpMatch;
  const values: RegExp?[] = [null, re];
  const RegExp: (string) => int = (source: string): int => source.length;
  return (re is RegExp) && (copy is RegExpMatch) && copy == found && values[1] == re &&
    RegExp('42') == 2 ? 42 : 0;
})",
                             R"(
func main(): int {
  const re = RegExp('(?<x>a)');
  const first = re.firstMatch;
  const prefix: (string) => RegExpMatch? = re.matchAsPrefix;
  return first('a')?.group(1) == 'a' && prefix('a')?.start == 0 ? 42 : 0;
})"));

class RegExpRejectedTest : public testing::TestWithParam<std::string_view> {};

TEST(RegExpFailureTraceTest, PreservesNativeDiagnosticAndSourceLocation) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(
      "func fail(): void {\n  RegExp('中(');\n}\nfunc main(): void { fail(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  EXPECT_EQ(result.status, ExecutionStatus::RegExpSyntaxError);
  ASSERT_EQ(result.stack_trace.size(), 2);
  EXPECT_EQ(result.stack_trace.front().function, "fail");
  EXPECT_EQ(result.stack_trace.front().location.line, 2);
  EXPECT_NE(result.message.find("offset 2"), std::string::npos);
}

TEST_P(RegExpRejectedTest, RejectsInvalidStaticContract) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StaticContracts,
    RegExpRejectedTest,
    testing::Values("func main(): void { RegExpMatch(); }",
                    "func main(): void { RegExp('x', unicode: false); }",
                    "func main(): void { RegExp(7); }",
                    "func main(): void { RegExp('x', multiLine: null); }",
                    "func main(): void { const r = RegExp('x'); r.pattern = 'y'; }",
                    "func main(): void { const r = RegExp('x'); r.isUnicode = false; }",
                    "func main(m: RegExpMatch): void { m[0] = 'x'; }",
                    "func main(m: RegExpMatch): void { m.start++; }",
                    "func main(m: RegExpMatch): void { m.end += 1; }",
                    "func main(m: RegExpMatch): void { m.group('x'); }",
                    "func main(m: RegExpMatch): void { const a: string[] = m.groups([0]); }",
                    "func main(r: RegExp): void { const a: RegExp[] = r.allMatches('x'); }",
                    "func main(r: RegExp?): void { r.hasMatch('x'); }",
                    "func main(r: RegExp): void { r.hasMatch = (s: string): bool => true; }",
                    "class RegExp {}",
                    "class RegExpMatch {}"));

}  // namespace
}  // namespace vela::compiler
