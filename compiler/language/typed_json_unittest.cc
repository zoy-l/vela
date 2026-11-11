#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "compiler/frontend/diagnostic.h"
#include "compiler/test_support/execution_expectations.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/value/value.h"

namespace vela::compiler {
namespace {

class TypedJSONProgramTest : public testing::TestWithParam<std::string_view> {};

TEST_P(TypedJSONProgramTest, ExecutesAndReleasesSessionStateWithGC) {
  test::ExpectReleasedAnswer(GetParam());
}

TEST_P(TypedJSONProgramTest, RestoresWithoutCompilerState) {
  test::ExpectRestoredAnswer(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    DecodeContracts,
    TypedJSONProgramTest,
    testing::Values(
        R"(func main(): int {
  const result = JSON.safeParse<int>('42');
  const number: int = result.value;
  const absent = JSON.safeParse<string?>('null');
  return result.success && absent.success && absent.value == null && number == 42 ? 42 : 0;
})",
        R"(func main(): int {
  const result = JSON.safeParse<int>('1.0');
  const error = result.error;
  if (result.success || error.code != 'type_mismatch' || error.path != '$' ||
      error.expected != 'int' || error.actual != 'double' || error.offset != 0) { return 0; }
  try { result.value; } catch (wrong: Error) {
    return wrong.code == 'TypeError' ? 42 : 0;
  }
  return 0;
})",
        R"(class User { name: string; age: int = 42; nickname: string? = null; }
func main(): int {
  const result = JSON.safeParse<User>('{"name":"Vela"}');
  const user: User = result.value;
  if (!result.success || user.name != 'Vela' || user.nickname != null) { return 0; }
  try { result.error; } catch (error: Error) { return error.code == 'TypeError' ? user.age : 0; }
  return 0;
})",
        R"(let calls: int = 0;
func next(): int { calls += 1; return calls; }
class Item { name: string; id: int = next(); values: int[] = []; }
func main(): int {
  const values = JSON.parse<Item[]>('[{"name":"a"},{"name":"b","id":40}]');
  values[0].values.push(42);
  return calls == 1 && values[0].id == 1 && values[1].id == 40 &&
      values[1].values.length == 0 ? values[0].values[0] : 0;
})",
        R"(let calls: int = 0;
class Inner { value: int; constructor { calls += 1; } }
class Outer { first: Inner; last: int; }
func main(): int {
  const failed = JSON.safeParse<Outer>('{"first":{"value":1},"last":false}');
  return !failed.success && failed.error.path == '$.last' && calls == 0 ? 42 : 0;
})",
        R"(class Secret {
  private value: int = 0;
  constructor(input: int) { this.value = input; }
  read(): int { return this.value; }
}
func main(): int {
  const good = JSON.parse<Secret>('{"input":42}');
  const bad = JSON.safeParse<Secret>('{"value":42}');
  return good.read() == 42 && bad.error.code == 'missing_field' &&
      bad.error.path == '$.input' && bad.error.offset == null ? 42 : 0;
})",
        R"(class Checked {
  value: int;
  constructor(value: int) { throw Error(message: 'user failure', code: 'UserError'); }
}
func main(): int {
  try { JSON.safeParse<Checked>('{"value":42}'); } catch (error: Error) {
    return error.code == 'UserError' ? 42 : 0;
  }
  return 0;
})",
        R"(class Node<T> { value: T; children: Node<T>[] = []; }
func decode<T>(source: string): T { return JSON.parse<T>(source); }
func main(): int {
  const parse: (string) => Node<int> = JSON.parse<Node<int>>;
  const safe: (string) => JSONParseResult<int> = JSON.safeParse<int>;
  const root = parse('{"value":20,"children":[{"value":22}]}');
  return safe('42').value == 42 && decode<int>('42') == 42
      ? root.value + root.children[0].value : 0;
})",
        R"(type Items = Map<string, int[]>;
func main(): int {
  const items = JSON.parse<Items>('{"a":[1],"a":[40,2],"b":[]}');
  const values = items.get('a');
  return items is Items && values != null && JSON.stringify(items) == '{"a":[40,2],"b":[]}'
      ? values[0] + values[1] : 0;
})",
        R"(class User { name: string; age: int; }
func main(): int {
  const missing = JSON.safeParse<User>('{"wrong":false}');
  const unknown = JSON.safeParse<User>('{"name":"a","age":1,"wrong":false}');
  const bad = JSON.safeParse<User>('{"age":false,"name":1}');
  return missing.error.path == '$.name' && missing.error.actual == 'missing' &&
      unknown.error.path == '$.wrong' && unknown.error.code == 'unknown_field' &&
      bad.error.path == '$.name' ? 42 : 0;
})",
        R"(func main(): int {
  const failure = JSON.safeParse<Map<string, int>>('{"display.name":false}');
  const error = failure.error;
  return error.path == '$["display.name"]' && error.offset == 16 ? 42 : 0;
})",
        R"(func main(): int {
  const syntax = JSON.safeParse<int>('{');
  const overflow = JSON.safeParse<int>('9223372036854775808');
  const hidden = JSON.safeParse<dynamic>('{"x":9223372036854775808,"x":1}');
  const minimum = JSON.parse<int>('-9223372036854775808');
  const negativeZero = JSON.parse<double>('-0.0');
  return syntax.error.code == 'syntax_error' && overflow.error.code == 'number_out_of_range' &&
      hidden.error.code == 'number_out_of_range' && minimum == -9223372036854775808 &&
      JSON.stringify(negativeZero) == '-0.0' ? 42 : 0;
})",
        R"(class Raised {
  constructor() { throw JSON.safeParse<int>('false').error; }
}
func main(): int {
  try { JSON.safeParse<Raised>('{}'); } catch (error: JSONParseError) {
    return error.code == 'type_mismatch' ? 42 : 0;
  }
  return 0;
})",
        R"(func main(): int {
  try { JSON.parse<int>('false'); } catch (error: JSONParseError) {
    const result = JSON.safeParse<int>('false');
    return error.code == result.error.code && error.path == result.error.path &&
        error.expected == result.error.expected && error.actual == result.error.actual &&
        error.offset == result.error.offset ? 42 : 0;
  }
  return 0;
})",
        R"(class Base { value: int; }
class Child extends Base {
  bonus: int;
  constructor { super(value: this.bonus * 20); }
}
func main(): int {
  const child = JSON.parse<Child>('{"bonus":2}');
  const rejected = JSON.safeParse<Child>('{"bonus":2,"value":40}');
  return rejected.error.code == 'unknown_field' ? child.value + child.bonus : 0;
})",
        R"(class User { name: string?; age: int? = 42; }
func main(): int {
  const absent = JSON.safeParse<User>('{}');
  const present = JSON.parse<User>('{"name":null,"age":null}');
  return absent.error.code == 'missing_field' && present.name == null && present.age == null ? 42 : 0;
})",
        R"(func source(): string { throw Error(message: 'source', code: 'SourceError'); }
func main(): int {
  try { JSON.safeParse<int>(source()); } catch (error: Error) {
    return error.code == 'SourceError' ? 42 : 0;
  }
  return 0;
})",
        R"(func main(): int {
  const erased: dynamic = JSON.safeParse<int>('42');
  try { erased.success = false; } catch (error: Error) {
    return erased.success && erased.value == 42 ? 42 : 0;
  }
  return 0;
})",
        R"(class Fields {
  value: int = 42;
  private secret: int = 0;
  static shared: int = 0;
  get computed(): int { return 1; }
}
func main(): int {
  const privateKey = JSON.safeParse<Fields>('{"secret":1}');
  const staticKey = JSON.safeParse<Fields>('{"shared":1}');
  const getterKey = JSON.safeParse<Fields>('{"computed":1}');
  const error: dynamic = privateKey.error;
  try { error.path = '$.changed'; } catch (write: Error) {
    return error.code == 'unknown_field' && error.path == '$.secret' &&
        error.expected == null && error.actual == 'int' && error.offset == 1 &&
        staticKey.error.code == 'unknown_field' &&
        getterKey.error.code == 'unknown_field' ? 42 : 0;
  }
  return 0;
})",
        R"(class Value { value: int; }
func main(): int {
  const escaped = JSON.parse<Value>('{"val\\u0075e":42}');
  const syntax = JSON.safeParse<Value>('{"other":0,"value":false,"last":[}');
  const number = JSON.safeParse<Value>('{"other":0,"value":false,"last":1e999}');
  const first = JSON.safeParse<Value>('{"first":1,"second":2,"value":false}');
  return escaped.value == 42 && syntax.error.code == 'syntax_error' &&
      number.error.code == 'number_out_of_range' && first.error.path == '$.first' ? 42 : 0;
})",
        R"(class Value { value: string?; }
func main(): int {
  const scalar = JSON.safeParse<int?>('false');
  const array = JSON.safeParse<int[]?>('false');
  const field = JSON.safeParse<Value?>('{"value":42}');
  const missing = JSON.safeParse<Value?>('{}');
  const present = JSON.parse<Value?>('{"value":null}');
  return scalar.error.expected == 'int?' && array.error.expected == 'int[]?' &&
      field.error.expected == 'string?' && missing.error.expected == 'string?' &&
      present != null && present.value == null ? 42 : 0;
})",
        R"(class Json { static read(): int { return 42; } }
func main(): int {
  const parse: (string) => dynamic = JSON.parse;
  const value = parse('42');
  return (value is int) ? Json.read() : 0;
})"));

class TypedJSONDiagnosticTest : public testing::TestWithParam<std::string_view> {};

TEST(TypedJSONExecutionTest, PreservesPresenceMasksAcrossMultipleWords) {
  std::string source = "class Wide { ";
  constexpr std::size_t kFieldCount = 70;
  for (std::size_t index = 0; index < kFieldCount; ++index) {
    source += "field" + std::to_string(index) + ": int = 1; ";
  }
  source += R"(} func main(): int {
    const value = JSON.parse<Wide>('{"field0":20,"field69":22}');
    return value.field1 == 1 && value.field68 == 1 ? value.field0 + value.field69 : 0;
  })";
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(source, isolate);
}

TEST(TypedJSONExecutionTest, MaterializesDeepRecursiveClassesWithBoundedNativeStack) {
  constexpr std::size_t kDepth = 1500;
  const auto source = std::string(R"(class Node { next: Node? = null; }
func main(): int {
  let node: Node? = JSON.parse<Node>(')") +
                      [&] {
                        std::string input;
                        for (std::size_t depth = 0; depth < kDepth; ++depth) {
                          input += "{\"next\":";
                        }
                        input += "null";
                        input.append(kDepth, '}');
                        return input;
                      }() +
                      R"(');
  let count = 0;
  while (node != null) { count += 1; node = node.next; }
  return count == 1500 ? 42 : 0;
})";
  runtime::Isolate isolate(1);
  test::ExpectRunAnswer(source, isolate);
}

TEST(TypedJSONExecutionTest, ReleasesFailedAllocationsAndReusesTheSameInstance) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileSource(R"(
class User { name: string; values: int[] = []; }
export func decode(source: string): int {
  const result = JSON.safeParse<User>(source);
  if (result.success) { return result.value.name == 'Vela' ? 42 : 0; }
  return result.error.code == 'syntax_error' || result.error.code == 'type_mismatch' ? 42 : 0;
}

)",
                                            isolate);
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  const auto function = compiled.instance->FindFunction("decode");
  if (!function) {
    FAIL() << "Missing decode entry";
  }
  const auto entry = function.value();
  auto& heap = isolate.heap();
  for (const auto* const source : {R"({"name":"Vela"})", R"({"name":false})", "{"}) {
    const auto input_storage =
        runtime::HostRoot::Create(isolate, runtime::Value::String(heap.AllocateString(source)));
    ASSERT_TRUE(input_storage);
    const auto& input = *input_storage;
    const std::array arguments{input.value()};
    heap.FinishCollection();
    heap.Collect();
    const auto baseline = heap.allocated_bytes();
    heap.ResetAllocationStatistics();
    const auto measured = runtime::ExecuteNativeFunction(*compiled.instance, entry, arguments);
    ASSERT_TRUE(measured.Succeeded()) << measured.message;
    const auto allocations = heap.allocation_statistics().count;
    ASSERT_GT(allocations, 0U);
    heap.FinishCollection();
    heap.Collect();
    for (std::uint64_t allowance = 0; allowance < allocations; ++allowance) {
      heap.ResetAllocationStatistics();
      heap.SetAllocationCountLimit(allowance);
      const auto failed = runtime::ExecuteNativeFunction(*compiled.instance, entry, arguments);
      heap.SetAllocationCountLimit(std::numeric_limits<std::uint64_t>::max());
      EXPECT_EQ(failed.status, ExecutionStatus::OutOfMemory) << source << ": " << allowance;
      heap.FinishCollection();
      heap.Collect();
      EXPECT_EQ(heap.allocated_bytes(), baseline) << allowance;
      const auto reused = runtime::ExecuteNativeFunction(*compiled.instance, entry, arguments);
      ASSERT_TRUE(reused.Succeeded()) << reused.message;
      EXPECT_EQ(reused.value.AsInteger(), 42);
      heap.FinishCollection();
      heap.Collect();
      EXPECT_EQ(heap.allocated_bytes(), baseline) << allowance;
    }
  }
}

TEST(TypedJSONExecutionTest, ScalarSessionAllocationsDoNotDependOnUnrelatedTypePlans) {
  std::array<runtime::HeapAllocationStatistics, 2> statistics;
  for (std::size_t variant = 0; variant < statistics.size(); ++variant) {
    std::string source = "export func decode(): int { return JSON.parse<int>('42'); } ";
    if (variant != 0) {
      source += "class Unrelated { ";
      constexpr std::size_t kFieldCount = 64;
      for (std::size_t index = 0; index < kFieldCount; ++index) {
        source += "field" + std::to_string(index) + ": int = 0; ";
      }
      source += "} export func unused(): Unrelated { return JSON.parse<Unrelated>('{}'); }";
    }
    runtime::Isolate isolate;
    const auto compiled = test::CompileSource(source, isolate);
    ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
    const auto function = compiled.instance->FindFunction("decode");
    if (!function) {
      FAIL() << "Missing decode entry";
    }
    auto& heap = isolate.heap();
    heap.Collect();
    heap.ResetAllocationStatistics();
    const auto result = runtime::ExecuteNativeFunction(*compiled.instance, *function);
    ASSERT_TRUE(result.Succeeded()) << result.message;
    EXPECT_EQ(result.value.AsInteger(), 42);
    statistics[variant] = heap.allocation_statistics();
  }
  EXPECT_EQ(statistics.front().bytes, statistics.back().bytes);
  EXPECT_EQ(statistics.front().count, statistics.back().count);
}

void StopJSONExecution(runtime::NativeContext& context) noexcept {
  context.isolate().native_control().RequestStop();
}

TEST(TypedJSONExecutionTest, PropagatesStopRequestsFromConstructors) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<StopJSONExecution>("stop")));
  const auto compiled = test::CompileSource(R"(
external func stop(): void;
class Item { value: int; constructor { stop(); } }
func main(): int {
  const result = JSON.safeParse<Item[]>('[{"value":1},{"value":2}]');
  return result.success ? 42 : 0;
})",
                                            isolate);
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  const auto result = test::Execute(*compiled.instance);
  EXPECT_EQ(result.status, ExecutionStatus::ExecutionStopped) << result.message;
}

TEST(TypedJSONExecutionTest, PropagatesConstructorStackLimitsAndReusesTheInstance) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileSource(R"(
func recurse(depth: int): int { return depth + recurse(depth + 1); }
class Item { value: int = recurse(0); }
export func decode(): int { return JSON.safeParse<Item>('{}').value.value; }
export func plain(): int { return JSON.parse<int>('42'); }
)",
                                            isolate);
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  const auto decode = compiled.instance->FindFunction("decode");
  const auto plain = compiled.instance->FindFunction("plain");
  if (!decode || !plain) {
    FAIL() << "Missing execution entry";
  }
  const auto decode_entry = *decode;
  const auto plain_entry = *plain;
  const auto failed = runtime::ExecuteNativeFunction(*compiled.instance, decode_entry);
  EXPECT_EQ(failed.status, ExecutionStatus::StackOverflow) << failed.message;
  const auto reused = runtime::ExecuteNativeFunction(*compiled.instance, plain_entry);
  ASSERT_TRUE(reused.Succeeded()) << reused.message;
  EXPECT_EQ(reused.value.AsInteger(), 42);
}

TEST_P(TypedJSONDiagnosticTest, RejectsInvalidTargetAndAPIContracts) {
  test::ExpectRejectedSource(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    StaticContracts,
    TypedJSONDiagnosticTest,
    testing::Values(
        "func main(): void { JSON.safeParse('42'); }",
        "func main(): void { const parse: (string) => JSONParseResult<int> = JSON.safeParse; }",
        "func main(): void { JSON.parse<Set<int>>('[]'); }",
        "func main(): void { JSON.parse<Map<int, string>>('{}'); }",
        "func main(): void { JSON.parse<Future<int>[]>('[]'); }",
        "interface User {} func main(): void { JSON.safeParse<User?>('null'); }",
        "enum Role { one } func main(): void { JSON.safeParse<Role[]>('[]'); }",
        "class User { private constructor() {} } func main(): void { JSON.parse<User>('{}'); }",
        "class User { value: Set<int>; } func main(): void { JSON.parse<User[]>('[]'); }",
        "func main(): void { JSON.parse<(int) => int>('null'); }",
        "func main(): void { JSON.safeParse<int>(42); }",
        "func unused<T>(): void { JSON.parse<Set<int>>('[]'); } func main(): void {}",
        "func main(): void { JSON.safeParse<((int) => int)?>('null'); }",
        "func main(): void { const result = JSON.safeParse<int>('42'); result.success = false; }",
        "func main(): void { const result = JSON.safeParse<int>('42'); result.value = 7; }",
        "func main(): void { const error = JSON.safeParse<int>('false').error; error.code = ''; }",
        "func main(): void { JSONParseResult<int>(); }",
        "func main(): void { JSONParseError(); }",
        "func main(): void { Json.parse('42'); }",
        "class Grow<T> { next: Grow<T[]>?; } "
        "func main(): void { JSON.safeParse<Grow<int>>('{}'); }",
        "class JSON {} func main(): void {}"));

}  // namespace
}  // namespace vela::compiler
