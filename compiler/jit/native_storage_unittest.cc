#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/native/native_values.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

std::string_view CollectText(runtime::NativeContext& context) noexcept {
  context.isolate().heap().Collect();
  return "retained";
}

class NativeStorageTest : public testing::TestWithParam<NativeCodeTier> {};

TEST_P(NativeStorageTest, IsolatesScalarCallResultsWhilePreservingGcAndFailurePrefixes) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled =
      CompileSource(**compiler, R"(
external func text(): string;
func increment(value: int): int { return value + 1; }
func flip(value: bool): bool { return !value; }
func twice(value: double): double { return value * 2.0; }
export func run(): int {
  const retained = text();
  const count = increment(20);
  const scalar = twice(10.5);
  const flag = flip(false);
  text();
  return flag && scalar == 21.0 ? retained.length + count + scalar.toInt() : -1;
}
export func prefix(initial: int): int {
  let result = initial;
  try {
    result = increment(result);
    result = increment(result);
    result = increment(result);
  } catch (error: Error) { text(); return result; }
  return result;
}
export func fail(): int { return increment(9223372036854775807); }
)",
                    {.diagnostics = {}, .compilation = {.tier = GetParam()}, .reflection = {}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&CollectText>("text")));
  auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_TRUE(instance) << instance.error().message;
  runtime::NativeExecution execution(**instance);
  const auto run = (*instance)->FindFunction("run");
  const auto prefix = (*instance)->FindFunction("prefix");
  const auto fail = (*instance)->FindFunction("fail");
  if (!run || !prefix || !fail) {
    FAIL() << "expected exported functions run, prefix and fail";
  }
  const auto value = runtime::InvokeNativeFunction(execution, *run);
  ASSERT_TRUE(value);
  EXPECT_EQ(value->AsInteger(), 50);
  constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
  const std::array arguments{runtime::Value::Integer(kMaximum - 1)};
  const auto preserved = runtime::InvokeNativeFunction(execution, *prefix, arguments);
  ASSERT_TRUE(preserved);
  EXPECT_EQ(preserved->AsInteger(), kMaximum);
  const auto failed = runtime::ExecuteNativeFunction(**instance, *fail);
  EXPECT_EQ(failed.status, ExecutionStatus::IntegerOverflow);
  ASSERT_EQ(failed.stack_trace.size(), 2U);
  EXPECT_EQ(failed.stack_trace.front().function, "increment");
  EXPECT_EQ(failed.stack_trace.back().function, "fail");
}

TEST_P(NativeStorageTest, IsolatesScalarResultsInFramesWithExternalStorage) {
  constexpr std::size_t kSubstitutions = 300;
  std::string source =
      "external func text(): string; "
      "func increment(value: int): int { return value + 1; } "
      "export func run(value: int): int { const retained = text(); const combined = '";
  for (std::size_t index = 0; index < kSubstitutions; ++index) {
    source += "${value}";
  }
  source +=
      "'; const count = increment(value); text(); "
      "return combined.length + count + retained.length; }";
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled =
      CompileSource(**compiler, source,
                    {.diagnostics = {}, .compilation = {.tier = GetParam()}, .reflection = {}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  EXPECT_TRUE(std::ranges::any_of(compiled.code.function_metrics, [](const auto& metrics) {
    return metrics.frame_bytes > native::kMaximumNativeStackBytes;
  }));
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&CollectText>("text")));
  auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_TRUE(instance) << instance.error().message;
  const auto run = (*instance)->FindFunction("run");
  if (!run) {
    FAIL() << "expected exported function run";
  }
  const std::array arguments{runtime::Value::Integer(1)};
  const auto result = runtime::ExecuteNativeFunction(**instance, *run, arguments);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), static_cast<std::int64_t>(kSubstitutions) + 10);
}

TEST_P(NativeStorageTest, WordRotationsRetainWideInputsShiftCountsAndCheckedFailures) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled =
      CompileSource(**compiler, R"(
func rotate(value: int, bits: int): int {
  return ((value >>> bits) | (value << (32 - bits))) & 4294967295;
}
export func bounded(value: int): int { return rotate(value & 4294967295, 7); }
export func wide(value: int, bits: int): int { return rotate(value, bits); }
export func iterate(seed: int, count: int): int {
  let value = seed & 4294967295;
  for (let i = 0; i < count; i++) { value = (rotate(value, 7) + 42) & 4294967295; }
  return value;
}
export func checked(value: int): int { return rotate((value + 1) & 4294967295, 7); }
)",
                    {.diagnostics = {}, .compilation = {.tier = GetParam()}, .reflection = {}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate isolate(1);
  auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_TRUE(instance) << instance.error().message;
  const auto bounded = (*instance)->FindFunction("bounded");
  const auto wide = (*instance)->FindFunction("wide");
  const auto iterate = (*instance)->FindFunction("iterate");
  const auto checked = (*instance)->FindFunction("checked");
  if (!bounded || !wide || !iterate || !checked) {
    FAIL() << "expected exported functions bounded, wide, iterate and checked";
  }
  constexpr auto kWordBits = std::numeric_limits<std::uint32_t>::digits;
  constexpr std::int64_t kMask = std::numeric_limits<std::uint32_t>::max();
  constexpr std::int64_t kWideBit = std::int64_t{1} << kWordBits;
  constexpr std::int64_t kIterations = 257;
  constexpr unsigned kRotateBits = 7;
  constexpr unsigned kIncrement = 42;
  constexpr auto kInputs = std::to_array<std::int64_t>({
      0,
      1,
      -1,
      kMask,
      kWideBit,
      std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max(),
  });
  constexpr auto kShifts = std::to_array<std::int64_t>({
      -1,
      0,
      1,
      kRotateBits,
      kWordBits - 1,
      kWordBits,
      kWordBits + 1,
      std::int64_t{2} * kWordBits,
  });
  for (const auto input : kInputs) {
    const auto initial = static_cast<std::uint32_t>(input);
    const std::array one{runtime::Value::Integer(input)};
    const auto rotated = runtime::ExecuteNativeFunction(**instance, *bounded, one);
    ASSERT_TRUE(rotated.Succeeded()) << rotated.message;
    EXPECT_EQ(rotated.value.AsInteger(), std::rotr(initial, kRotateBits));
    auto expected_loop = initial;
    for (std::int64_t iteration = 0; iteration < kIterations; ++iteration) {
      expected_loop = std::rotr(expected_loop, kRotateBits) + kIncrement;
    }
    const std::array loop_arguments{
        runtime::Value::Integer(input),
        runtime::Value::Integer(kIterations),
    };
    const auto loop = runtime::ExecuteNativeFunction(**instance, *iterate, loop_arguments);
    ASSERT_TRUE(loop.Succeeded()) << loop.message;
    EXPECT_EQ(loop.value.AsInteger(), expected_loop);
    for (const auto shift : kShifts) {
      constexpr auto kShiftMask = std::numeric_limits<std::uint64_t>::digits - 1;
      const auto bits = std::bit_cast<std::uint64_t>(input);
      const auto right = static_cast<std::uint64_t>(shift) & kShiftMask;
      const auto left = static_cast<std::uint64_t>(kWordBits - shift) & kShiftMask;
      const auto expected = ((bits >> right) | (bits << left)) & kMask;
      const std::array arguments{runtime::Value::Integer(input), runtime::Value::Integer(shift)};
      const auto result = runtime::ExecuteNativeFunction(**instance, *wide, arguments);
      ASSERT_TRUE(result.Succeeded()) << result.message;
      EXPECT_EQ(result.value.AsInteger(), expected);
    }
  }
  const std::array maximum{runtime::Value::Integer(std::numeric_limits<std::int64_t>::max())};
  const auto failed = runtime::ExecuteNativeFunction(**instance, *checked, maximum);
  EXPECT_EQ(failed.status, ExecutionStatus::IntegerOverflow);
}

TEST_P(NativeStorageTest, ParallelFieldUpdatesRetainAliasingAndStrictFloatingPointResults) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled =
      CompileSource(**compiler, R"(
class Particle { x: double; y: double; z: double; vx: double; vy: double; vz: double; }
func update(points: Particle[]): void {
  for (const point in points) {
    point.x += point.vx;
    point.y += point.vy;
    point.z += point.vz;
  }
}
export func run(): double {
  const point = Particle(x: 1.0, y: 2.0, z: 3.0, vx: 4.0, vy: 5.0, vz: 6.0);
  update([point, point]);
  if (point.x != 9.0 || point.y != 12.0 || point.z != 15.0) { return 1.0; }
  const infinity = 1e308 * 1e308;
  const edge = Particle(x: -0.0, y: infinity, z: infinity - infinity,
                        vx: -0.0, vy: -infinity, vz: 1.0);
  update([edge]);
  return Math.isNaN(edge.y) && Math.isNaN(edge.z) ? edge.x : 1.0;
}
)",
                    {.diagnostics = {}, .compilation = {.tier = GetParam()}, .reflection = {}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate isolate(1);
  auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_TRUE(instance) << instance.error().message;
  const auto run = (*instance)->FindFunction("run");
  if (!run) {
    FAIL() << "expected exported function run";
  }
  const auto result = runtime::ExecuteNativeFunction(**instance, *run);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_DOUBLE_EQ(result.value.AsDouble(), 0.0);
  EXPECT_TRUE(std::signbit(result.value.AsDouble()));
}

TEST_P(NativeStorageTest, FieldUpdatesKeepHostVisibleKindsAndGcReferencesAfterConstruction) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled =
      CompileSource(**compiler, R"(
external func text(): string;
class Fields {
  scalar: double;
  flag: bool;
  count: int;
  optional: double? = null;
  any: dynamic = null;
  label: string;
  constructor(seed: double) {
    this.scalar = seed; this.flag = false; this.count = 0; this.label = text();
  }
  update(): void {
    this.scalar += 1.0; this.flag = !this.flag; this.count += 2;
    this.optional = 3.0; this.optional = null; this.any = 4.0; this.any = text();
  }
}
export func run(): Fields {
  const fields = Fields(1.0); fields.update(); text(); return fields;
}
)",
                    {.diagnostics = {}, .compilation = {.tier = GetParam()}, .reflection = {}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&CollectText>("text")));
  auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_TRUE(instance) << instance.error().message;
  const auto run = (*instance)->FindFunction("run");
  ASSERT_TRUE(run);
  const auto result = runtime::ExecuteNativeFunction(**instance, *run);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  runtime::RootRange root(isolate.heap(), &result.value, 1);
  isolate.heap().Collect();
  const auto& object = *result.value.AsObject();
  const auto scalar = runtime::Heap::LoadField(object, 0);
  const auto flag = runtime::Heap::LoadField(object, 1);
  const auto count = runtime::Heap::LoadField(object, 2);
  ASSERT_TRUE(scalar.IsDouble());
  EXPECT_DOUBLE_EQ(scalar.AsDouble(), 2.0);
  ASSERT_TRUE(flag.IsBoolean());
  EXPECT_TRUE(flag.AsBoolean());
  ASSERT_TRUE(count.IsInteger());
  EXPECT_EQ(count.AsInteger(), 2);
  EXPECT_TRUE(runtime::Heap::LoadField(object, 3).IsNull());
  for (const auto index : {4U, 5U}) {
    const auto text = runtime::Heap::LoadField(object, index);
    ASSERT_TRUE(text.IsString());
    EXPECT_EQ(text.AsString()->value(), "retained");
  }
}

INSTANTIATE_TEST_SUITE_P(Tiers,
                         NativeStorageTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::compiler::jit
