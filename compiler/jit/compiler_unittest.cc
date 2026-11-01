#include "compiler/jit/compiler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/code_inputs.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/test_support.h"
#include "compiler/native/code_generator.h"
#include "compiler/test_support/test_sources.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/rooted_value.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_contracts.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

using frontend::DiagnosticsText;
using test::CompileText;

TEST(JitCompilerTest, OptimizedFieldLoopsPreserveInvariantInputsAcrossClonedRecoveryExits) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled = CompileText(**compiler, R"(
class Point { x: double = 0.0; y: double = 2.0; z: double = 2.0; }
class Child extends Point { padding: int = 7; }
func advance(points: Point[], rounds: int): double {
  for (let round = 0; round < rounds; round++) {
    for (const point in points) {
      point.x += point.y;
      point.z += point.y;
    }
  }
  return points[0].x + points[0].z;
}
export func main(): int {
  const points: Point[] = [Point()];
  const children: Point[] = [Child()];
  const first = advance(points, 10);
  const second = advance(children, 10);
  return first == 42.0 && second == 42.0 ? 42 : 0;
}
)",
                                    NativeCodeTier::Optimized);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  const auto main = isolate.native_dispatch().FindEntry("main.vela", "main");
  ASSERT_TRUE(main);
  ASSERT_TRUE(test::ReturnsInteger(*main, 42));
}

double HostSquareRoot(double value) noexcept {
  constexpr double kHostOverrideOffset = 40.0;
  return value + kHostOverrideOffset;
}

TEST(JitCompilerTest, StringLengthsUseUnicodeScalarsAndUtf8Bytes) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled = CompileText(**compiler, R"(
func lengths(text: string): int { return text.length * 100 + text.byteLength; }
export func main(): int {
  const text = 'A中😀e\u0301';
  return lengths(text) + lengths(text.slice(1, 3)) + lengths('');
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", "main");
    ASSERT_TRUE(entry);
    const auto result = entry->Invoke({});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->AsInteger(), 718);
  }
}

TEST(JitCompilerTest, PrivateIntegerArrayRangesPreserveEscapingWritesAndCheckedArithmetic) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    const auto compiled = CompileText(**compiler, R"(
class Box { values: int[]; }
func alter(values: int[], value: int): void { values[0] = value; }
func alterBox(box: Box, value: int): void { box.values[0] = value; }
export func masked(value: int): int {
  const values = [0]; const alias = values;
  alias[0] = value & 255;
  const text = value.toString();
  return text.length > 0 ? values[0] : 0;
}
export func escaped(value: int): int {
  const values = [0]; alter(values, value); return values[0];
}
export func borrowed(value: int): int {
  const values = [0]; const box = Box(values: values);
  alterBox(box, value); return values[0];
}
export func overflow(value: int): int {
  const values = [0]; values[0] = (value + 1) & 255; return values[0];
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    const auto masked = isolate.native_dispatch().FindEntry("main.vela", "masked");
    const auto escaped = isolate.native_dispatch().FindEntry("main.vela", "escaped");
    const auto borrowed = isolate.native_dispatch().FindEntry("main.vela", "borrowed");
    const auto overflow = isolate.native_dispatch().FindEntry("main.vela", "overflow");
    ASSERT_TRUE(masked);
    ASSERT_TRUE(escaped);
    ASSERT_TRUE(borrowed);
    ASSERT_TRUE(overflow);
    constexpr std::int64_t kWideValue = (std::int64_t{1} << 40) + 42;
    const std::array wide{runtime::Value::Integer(kWideValue)};
    const auto bounded = masked->Invoke(wide);
    ASSERT_TRUE(bounded);
    EXPECT_EQ(bounded->AsInteger(), 42);
    const auto unbounded = escaped->Invoke(wide);
    ASSERT_TRUE(unbounded);
    EXPECT_EQ(unbounded->AsInteger(), kWideValue);
    const auto nested = borrowed->Invoke(wide);
    ASSERT_TRUE(nested);
    EXPECT_EQ(nested->AsInteger(), kWideValue);
    const std::array maximum{runtime::Value::Integer(std::numeric_limits<std::int64_t>::max())};
    const auto failed = overflow->Invoke(maximum);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), ExecutionStatus::IntegerOverflow);
    const auto valid = overflow->Invoke(wide);
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->AsInteger(), 43);
  }
}

TEST(JitCompilerTest, BoundedLeafCallsRetainFailureFramesAndCallerRoots) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, R"(
class Box { text: string; }
func divide(value: int): int { return 84 / value; }
export func fail(): int { return divide(0); }
export func caught(): int {
  const box = Box(text: 42.toString());
  try { return divide(0); }
  catch (error: Error) { return error.code == 'DivisionByZero' ? box.text.length + 40 : 0; }
}
export func answer(): int { return divide(2); }
)",
                                    NativeCodeTier::Optimized);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  EXPECT_NE(compiled->llvm_ir.find("@vela_native_v2_leaf_failure"), std::string::npos);
  runtime::Isolate isolate(1);
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  auto instance = runtime::NativeInstance::Borrow(isolate.native_dispatch().current_state());
  const auto target = instance.FindFunction("fail");
  ASSERT_TRUE(target);
  const auto failed = runtime::ExecuteNativeFunction(instance, *target);
  EXPECT_EQ(failed.status, ExecutionStatus::DivisionByZero);
  ASSERT_EQ(failed.stack_trace.size(), 2U);
  EXPECT_EQ(failed.stack_trace.front().function, "divide");
  EXPECT_EQ(failed.stack_trace.back().function, "fail");
  for (const auto* const name : {"caught", "answer"}) {
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", name);
    ASSERT_TRUE(entry);
    ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
  }
}

TEST(JitCompilerTest, GuardedReadonlyLeafLoopsPreserveFramesAndLargeInputs) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled = CompileText(**compiler, R"(
class Box { text: string; }
func sum(values: int[], count: int): int {
  let result = 0;
  for (let index = 0; index < count; index++) { result += values[index]; }
  return result;
}
export func fail(): int { return sum([1], 2); }
export func caught(): int {
  const box = Box(text: 42.toString());
  try { return sum([1], 2); }
  catch (error: Error) { return error.code == 'IndexOutOfBounds' ? box.text.length + 40 : 0; }
}
export func run(count: int): int {
  const values: int[] = [];
  values.reserve(count);
  for (let index = 0; index < count; index++) { values.push(index); }
  return sum(values, count);
}
)",
                                    NativeCodeTier::Optimized);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  auto instance = runtime::NativeInstance::Borrow(isolate.native_dispatch().current_state());
  const auto target = instance.FindFunction("fail");
  ASSERT_TRUE(target);
  const auto failed = runtime::ExecuteNativeFunction(instance, *target);
  EXPECT_EQ(failed.status, ExecutionStatus::IndexOutOfBounds);
  ASSERT_EQ(failed.stack_trace.size(), 2U);
  EXPECT_EQ(failed.stack_trace.front().function, "sum");
  EXPECT_EQ(failed.stack_trace.back().function, "fail");
  const auto run = isolate.native_dispatch().FindEntry("main.vela", "run");
  const auto caught = isolate.native_dispatch().FindEntry("main.vela", "caught");
  ASSERT_TRUE(run);
  ASSERT_TRUE(caught);
  ASSERT_TRUE(test::ReturnsInteger(*caught, 42));
  for (const std::int64_t count : {0, 1, 31, 1024, 4096}) {
    const std::array arguments{runtime::Value::Integer(count)};
    ASSERT_TRUE(test::ReturnsInteger(*run, count * (count - 1) / 2, arguments));
  }
}

TEST(JitCompilerTest, ScalarTextLengthsPreserveFormattingWithoutAllocatingStrings) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled = CompileText(**compiler, R"(
export func length(value: double): int { return value.toString().length; }
export func text(value: double): string { return value.toString(); }
export func changed(): int { let value = 42; const text = value.toString(); value = 1000; return text.length; }
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    auto& dispatch = isolate.native_dispatch();
    const auto length = dispatch.FindEntry("main.vela", "length");
    const auto text = dispatch.FindEntry("main.vela", "text");
    const auto changed = dispatch.FindEntry("main.vela", "changed");
    ASSERT_TRUE(length);
    ASSERT_TRUE(text);
    ASSERT_TRUE(changed);
    ASSERT_TRUE(test::ReturnsInteger(*changed, 2));
    for (const auto value : {
             0.0,
             -0.0,
             1.5,
             std::numeric_limits<double>::denorm_min(),
             std::numeric_limits<double>::max(),
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
         }) {
      const std::array arguments{runtime::Value::Double(value)};
      const auto rendered = text->Invoke(arguments);
      ASSERT_TRUE(rendered);
      const auto expected = rendered->AsString()->length();
      const auto count = isolate.heap().object_count();
      ASSERT_TRUE(test::ReturnsInteger(*length, expected, arguments));
      EXPECT_EQ(isolate.heap().object_count(), count);
    }
  }
}

TEST(JitCompilerTest, SquareRootIntrinsicPreservesFloatingPointErrorsAndHostOverrides) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled = CompileText(**compiler, R"(
export func root(value: double): double { return Math.sqrt(value); }
export func failure(): string {
  try { return Math.sqrt(-1.0).toString(); }
  catch (error: Error) { return error.message; }
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    EXPECT_NE(compiled->llvm_ir.find("llvm.sqrt.f64"), std::string::npos);
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", "root");
    ASSERT_TRUE(entry);
    for (const auto value : {
             4.0,
             -0.0,
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
         }) {
      const auto result = entry->Invoke(std::array{runtime::Value::Double(value)});
      ASSERT_TRUE(result);
      const auto actual = result->AsDouble();
      if (std::isnan(value)) {
        EXPECT_TRUE(std::isnan(actual));
      } else {
        EXPECT_EQ(std::bit_cast<std::uint64_t>(actual),
                  std::bit_cast<std::uint64_t>(std::sqrt(value)));
      }
    }
    const auto failure = isolate.native_dispatch().FindEntry("main.vela", "failure");
    ASSERT_TRUE(failure);
    const auto failed = failure->Invoke({});
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->AsString()->value(), "sqrt argument must be nonnegative");

    runtime::Isolate overridden;
    ASSERT_TRUE(
        overridden.RegisterNative(runtime::NativeFunction::Bind<HostSquareRoot>("Math.sqrt")));
    static_cast<void>(overridden.FreezeBindings());
    ASSERT_TRUE(test::PublishImage(overridden, compiled->image));
    const auto replacement = overridden.native_dispatch().FindEntry("main.vela", "root");
    ASSERT_TRUE(replacement);
    const auto result = replacement->Invoke(std::array{runtime::Value::Double(2.0)});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->AsDouble(), 42.0);
  }
}

TEST(JitCompilerTest, MaterializesTheCompleteImageBeforePublishingNamedEntries) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler,
                                    "func twice(value: int): int { return value * 2; } "
                                    "export func answer(): int { return twice(21); }");
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
  EXPECT_NE(compiled->llvm_ir.find("@vela_native_v3_call_declaration"), std::string::npos);
  const auto functions = compiled->image->functions.values();
  const auto target = std::ranges::find(functions, "answer",
                                        [](const auto& function) { return function.name.text(); });
  ASSERT_NE(target, functions.end());
  EXPECT_EQ(target->identity.revision, 1U);
  runtime::Isolate isolate(1);
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "answer");
  ASSERT_TRUE(entry);
  ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
}

TEST(JitCompilerTest, ReusesUnchangedCodeAndInvalidatesTheCompleteDirectCallClosure) {
  constexpr std::int64_t kEditVersions = 8;
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    CodeReuseCache reuse;
    SourceSession source("main.vela");
    runtime::Isolate isolate(1);
    runtime::NativeEntry independent = nullptr;
    for (std::int64_t version = 1; version <= kEditVersions; ++version) {
      const auto prepared = test::PrepareText(
          source, "func step(value: int): int { return value + " + std::to_string(version) +
                      "; } export func answer(): int { return step(40); } "
                      "export func independent(): int { return 7; }");
      ASSERT_TRUE(prepared);
      const auto compiled = (*compiler)->Compile(prepared->snapshot().program,
                                                 {.tier = tier, .preserve_ir = true}, &reuse);
      ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
      const auto functions = compiled->image->functions.values();
      const auto cold = std::ranges::find(
          functions, "independent", [](const auto& function) { return function.name.text(); });
      ASSERT_NE(cold, functions.end());
      if (version == 1) {
        independent = cold->entry;
      } else {
        EXPECT_EQ(cold->entry, independent);
        EXPECT_EQ(compiled->compiled_functions, tier == NativeCodeTier::Baseline ? 1U : 2U);
        const auto index = static_cast<std::size_t>(cold - functions.begin());
        EXPECT_EQ(compiled->llvm_ir.find("@vela_jit_entry_" + std::to_string(index) + '_'),
                  std::string::npos);
      }
      ASSERT_TRUE(test::PublishImage(isolate, compiled->image, source.snapshot()->revision));
      ASSERT_TRUE(source.Commit(*prepared));
      const auto answer = isolate.native_dispatch().FindEntry("main.vela", "answer");
      const auto unchanged = isolate.native_dispatch().FindEntry("main.vela", "independent");
      ASSERT_TRUE(answer);
      ASSERT_TRUE(unchanged);
      ASSERT_TRUE(test::ReturnsInteger(*answer, 40 + version));
      ASSERT_TRUE(test::ReturnsInteger(*unchanged, 7));
      isolate.heap().Collect();
      EXPECT_LE((*compiler)->resource_counts().batches, 2U);
    }
  }
}

TEST(JitCompilerTest, ReusePlanningPinsCodeUntilTheCandidateOwnsItsEntries) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  SourceSession source("main.vela");
  const auto prepared = test::PrepareText(source, "export func answer(): int { return 42; }");
  ASSERT_TRUE(prepared);
  CodeReuseCache reuse;
  auto compiled = (*compiler)->Compile(prepared->snapshot().program, {}, &reuse);
  ASSERT_TRUE(compiled);
  {
    std::vector<FunctionCodeInputs> inputs;
    inputs.reserve(reuse.sources.size());
    for (const auto& function : reuse.sources) {
      inputs.push_back({.fingerprint = function.fingerprint, .dependencies = {}});
    }
    const auto plan = PlanCodeReuse(std::move(inputs), reuse);
    EXPECT_TRUE(std::ranges::all_of(plan.reused, [](auto reused) { return reused != 0; }));
    compiled->image.reset();
    EXPECT_EQ((*compiler)->resource_counts().images, 0U);
    EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
  }
  EXPECT_EQ((*compiler)->resource_counts().batches, 0U);
}

TEST(JitCompilerTest, LayoutEditsInvalidateTheirUsersWithoutRecompilingIndependentFunctions) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    CodeReuseCache reuse;
    runtime::NativeEntry independent = nullptr;
    std::shared_ptr<const runtime::NativeImage> previous;
    for (const auto* type : {"int", "bool", "int"}) {
      SourceSession source("main.vela");
      const auto integer = std::string_view(type) == "int";
      const auto* const value = integer ? "40" : "true";
      const auto* const condition = integer ? "box.value == 40" : "box.value";
      auto prepared = test::PrepareText(
          source,
          "export func independent(): int { return 7; }\nclass Box { value: " + std::string(type) +
              "; }\nexport func read(): int { const box = Box(value: " + value + "); return " +
              condition + " ? 42 : 0; }");
      ASSERT_TRUE(prepared) << DiagnosticsText(prepared.error().diagnostics);
      const auto compiled =
          (*compiler)->Compile(prepared->snapshot().program, {.tier = tier}, &reuse);
      ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
      const auto functions = compiled->image->functions.values();
      const auto unchanged = std::ranges::find(
          functions, "independent", [](const auto& function) { return function.name.text(); });
      ASSERT_NE(unchanged, functions.end());
      if (previous) {
        EXPECT_EQ(unchanged->entry, independent);
      }
      independent = unchanged->entry;
      // 本场景没有跨版本实例或捕获；布局编辑在编译缓存边界执行，运行时独立验证结果。
      runtime::Isolate current(1);
      ASSERT_TRUE(test::PublishImage(current, compiled->image, 0));
      const auto read = current.native_dispatch().FindEntry("main.vela", "read");
      ASSERT_TRUE(read);
      ASSERT_TRUE(test::ReturnsInteger(*read, 42));
      previous = compiled->image;
    }
  }
}

TEST(JitCompilerTest, NullableFieldAccessTracksReorderedClassTablesAcrossCodeReuse) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  CodeReuseCache reuse;
  std::shared_ptr<const runtime::NativeImage> previous;
  for (const auto* declaration : {
           "class Box { value: int; } class Other { flag: bool; }",
           "class Other { flag: bool; } class Box { padding: int = 7; value: int; }",
           "class Box { value: int; } class Other { flag: bool; }",
       }) {
    SourceSession source("main.vela");
    auto prepared = test::PrepareText(source, std::string(declaration) + R"(
export func read(box: Box?): int { return box == null ? 0 : box.value; }
export func answer(): int { return read(Box(value: 42)); }
)");
    ASSERT_TRUE(prepared) << DiagnosticsText(prepared.error().diagnostics);
    const auto compiled = (*compiler)->Compile(prepared->snapshot().program,
                                               {.tier = NativeCodeTier::Optimized}, &reuse);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image, 0));
    const auto answer = isolate.native_dispatch().FindEntry("main.vela", "answer");
    ASSERT_TRUE(answer);
    ASSERT_TRUE(test::ReturnsInteger(*answer, 42));
    const auto read = isolate.native_dispatch().FindEntry("main.vela", "read");
    ASSERT_TRUE(read);
    const std::array null{runtime::Value::Null()};
    const auto absent = read->Invoke(null);
    ASSERT_TRUE(absent);
    EXPECT_EQ(absent->AsInteger(), 0);
    previous = compiled->image;
  }
}

TEST(JitCompilerTest, OptimizedFieldGuardsPreservePriorEffectsAndStoreRecoveryInputs) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled = CompileText(**compiler, R"(
class Base { value: int = 0; text: string = 'initial'; }
class Child extends Base { padding: int = 7; }
let effects = 0;
func store(value: Base, text: string): int {
  effects++;
  const answer = effects + 40;
  value.value = answer;
  value.text = text;
  return answer;
}
func read(value: Base): int { return value.value + value.text.length; }
export func main(): int {
  const value = Child();
  const text = 2.toString();
  const answer = store(value, text);
  return answer == 41 && effects == 1 && read(value) == 42 ? 42 : 0;
}
)",
                                    NativeCodeTier::Optimized);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  const auto main = isolate.native_dispatch().FindEntry("main.vela", "main");
  ASSERT_TRUE(main);
  ASSERT_TRUE(test::ReturnsInteger(*main, 42));
}

TEST(JitCompilerTest, WarmGlobalBindingsStopUsingSlotsWhenTheDispatchCloses) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    const auto compiled = CompileText(
        **compiler, "let value = 40; export func answer(): int { return value + 2; }", tier);
    ASSERT_TRUE(compiled);
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    auto& dispatch = isolate.native_dispatch();
    const auto entry = dispatch.FindEntry("main.vela", "answer");
    ASSERT_TRUE(entry);
    const auto retained = entry->Capture();
    ASSERT_TRUE(retained);
    ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
    ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
    dispatch.Close();
    const auto stopped = retained->Invoke({});
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error(), ExecutionStatus::ExecutionStopped);
  }
}

TEST(JitCompilerTest, InferredGenericsExecuteInBothNativeTiers) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled = CompileText(**compiler, R"(
func identity<T>(value: T): T { return value; }
class Box<T> { value: T; }
export func answer(): int {
  const operation: (int) => int = identity;
  const box = Box(value: identity(40));
  return box.value + operation(2);
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", "answer");
    ASSERT_TRUE(entry);
    ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
  }
}

TEST(JitCompilerTest, GenericMethodsAndInterfaceSelectorsExecuteInBothNativeTiers) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    for (const auto source : {
             ::vela::compiler::test::kGenericMethodsSource,
             ::vela::compiler::test::kWrappedGenericMethodValuesSource,
         }) {
      auto compiler = Compiler::Create();
      ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
      const auto compiled = CompileText(**compiler, std::string(source), tier);
      ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
      runtime::Isolate isolate(1);
      ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
      const auto entry = isolate.native_dispatch().FindEntry("main.vela", "main");
      ASSERT_TRUE(entry);
      ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
    }
  }
}

TEST(JitCompilerTest, NativeImagesCompareUnusedGenericMethodContractsByIdentity) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const std::string entry = " export func answer(): int { return 42; }";
  const auto initial =
      CompileText(**compiler, "interface Mapper<T> { map<U extends int>(value: U): U; }" + entry);
  const auto renamed =
      CompileText(**compiler, "interface Mapper<A> { map<B extends int>(value: B): B; }" + entry);
  ASSERT_TRUE(initial) << DiagnosticsText(initial.error());
  ASSERT_TRUE(renamed) << DiagnosticsText(renamed.error());
  EXPECT_TRUE(runtime::CheckNativeNominalContracts(*initial->image, *renamed->image));
  for (const auto* declaration : {
           "interface Mapper<T> { map<U extends double>(value: U): U; }",
           "interface Mapper<T> { map<U extends int>(value: U): U[]; }",
       }) {
    const auto changed = CompileText(**compiler, declaration + entry);
    ASSERT_TRUE(changed) << DiagnosticsText(changed.error());
    const auto checked = runtime::CheckNativeNominalContracts(*initial->image, *changed->image);
    ASSERT_FALSE(checked);
    EXPECT_NE(checked.error().declaration, DeclarationId{});
    EXPECT_EQ(checked.error().status, runtime::NativeContractStatus::RequiresReload);
  }
}

TEST(JitCompilerTest, VirtualClassFieldsRemainRootedAcrossUnrelatedAllocations) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, R"(
class Point { x: int; text: string; }
export func read(): int {
  const point = Point(x: 40, text: 'a' + 'b');
  const values = ['unrelated'];
  return point.x + point.text.length + values.length - 1;
}
)",
                                    NativeCodeTier::Optimized);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  const auto functions = compiled->image->functions.values();
  const auto found = std::ranges::find(functions, "read",
                                       [](const auto& function) { return function.name.text(); });
  ASSERT_NE(found, functions.end());
  const auto index = static_cast<std::size_t>(found - functions.begin());
  EXPECT_EQ(compiled->function_metrics.at(index).eliminated_allocations, 1U);
  EXPECT_NE(compiled->llvm_ir.find("_virtual_"), std::string::npos);
  runtime::Isolate isolate(1);
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "read");
  ASSERT_TRUE(entry);
  ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
}

TEST(JitCompilerTest, AsyncResumptionKeepsItsMachineCodeAfterTheCompilerIsDestroyed) {
  runtime::Isolate isolate(1);
  {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled =
        CompileText(**compiler,
                    "async func later(): Future<int> { await sleep(1); return 42; } "
                    "export func start(): Future<int> { return later(); }");
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  }
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "start");
  ASSERT_TRUE(entry);
  const auto started = entry->Invoke({});
  ASSERT_TRUE(started);
  ASSERT_TRUE(test::CompletesWithInteger(isolate, *started, 42));
}

TEST(JitCompilerTest, VirtualObjectsEndBeforeAwaitAndLiveObjectsUseHeapContinuationState) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, R"(
class Box { value: int; text: string; }
export async func dead(): Future<int> {
  const box = Box(value: 40, text: 'a' + 'b');
  const result = box.value + box.text.length;
  await sleep(1);
  return result;
}
export async func live(): Future<int> {
  const box = Box(value: 40, text: 'a' + 'b');
  await sleep(1);
  return box.value + box.text.length;
}
)",
                                    NativeCodeTier::Optimized);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  const auto functions = compiled->image->functions.values();
  for (const auto* const name : {"dead", "live"}) {
    const auto found = std::ranges::find(functions, name,
                                         [](const auto& function) { return function.name.text(); });
    ASSERT_NE(found, functions.end());
    const auto& metrics =
        compiled->function_metrics.at(static_cast<std::size_t>(found - functions.begin()));
    EXPECT_EQ(metrics.eliminated_allocations, std::string_view(name) == "dead" ? 1U : 0U);
    EXPECT_EQ(metrics.retained_allocations, std::string_view(name) == "live" ? 1U : 0U);
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", name);
    ASSERT_TRUE(entry);
    const auto started = entry->Invoke({});
    ASSERT_TRUE(started);
    const runtime::RootedValue root(isolate, *started);
    isolate.heap().Collect();
    ASSERT_TRUE(test::CompletesWithInteger(isolate, root.value(), 42));
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
}

TEST(JitCompilerTest, ComparingVirtualAndOverlappingLoopInstancesPreservesFields) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const std::string source = R"(
class Box { value: int; ignored: int; }
export func read(): int {
  const first = Box(value: 14, ignored: 0);
  let previous = Box(value: 0, ignored: 0);
  let sum = 0;
  for (let index = 0; index < 3; index++) {
    const current = Box(value: index, ignored: 0);
    sum += previous.ignored;
    sum += first != current ? first.value : 0;
    previous = current;
  }
  return sum;
}
)";
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    const auto compiled = CompileText(**compiler, source, tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", "read");
    ASSERT_TRUE(entry);
    const auto result = entry->Invoke({});
    ASSERT_TRUE(result) << ExecutionStatusName(result.error());
    EXPECT_EQ(result->AsInteger(), 42);
  }
}

TEST(JitCompilerTest, DiscardedCompiledCandidatesRetireTheirOrcResources) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  {
    const auto compiled = CompileText(**compiler, "export func answer(): int { return 42; }");
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
    EXPECT_GT((*compiler)->resource_counts().functions, 0U);
  }
  EXPECT_EQ((*compiler)->resource_counts().batches, 0U);
  EXPECT_EQ((*compiler)->resource_counts().functions, 0U);
}

TEST(JitCompilerTest, CapturedMachineCodeKeepsItsBodyAndCallsTheLatestNamedImplementation) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first = test::PrepareText(source,
                                       "export func answer(): int { return 40; } "
                                       "export func caller(): int { return answer(); }");
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *first));
  const auto answer = dispatch.FindEntry("main.vela", "answer");
  const auto caller = dispatch.FindEntry("main.vela", "caller");
  ASSERT_TRUE(answer);
  ASSERT_TRUE(caller);
  std::optional<runtime::NativeVersionEntry> old_answer = *answer->Capture();
  std::optional<runtime::NativeVersionEntry> old_caller = *caller->Capture();
  const auto next = test::PrepareText(source,
                                      "export func answer(): int { return 42; } "
                                      "export func caller(): int { return answer() + 100; }");
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *next));
  EXPECT_EQ(answer->Invoke({})->AsInteger(), 42);
  EXPECT_EQ(caller->Invoke({})->AsInteger(), 142);
  EXPECT_EQ(old_answer->Invoke({})->AsInteger(), 40);
  EXPECT_EQ(old_caller->Invoke({})->AsInteger(), 42);
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().batches, 2U);
  old_answer.reset();
  old_caller.reset();
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
}

TEST(JitCompilerTest, GlobalAndStaticSlotsAreSharedByOldAndNewCodeAndPendingCandidates) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first =
      test::PrepareText(source,
                        "let score = 10; const title = 'old'; let cold = 20; "
                        "class Counter { static value: int = 4; } "
                        "export func answer(): int { "
                        "return score + Counter.value + (title == 'old' ? 1 : 1000); } "
                        "export func edit(): void { score = 42; Counter.value = 7; } "
                        "export func bump(): int { score++; return score; } "
                        "export func readCold(): int { return cold; }");
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *first));
  const auto answer = dispatch.FindEntry("main.vela", "answer");
  const auto edit = dispatch.FindEntry("main.vela", "edit");
  const auto bump = dispatch.FindEntry("main.vela", "bump");
  const auto cold = dispatch.FindEntry("main.vela", "readCold");
  ASSERT_TRUE(answer);
  ASSERT_TRUE(edit);
  ASSERT_TRUE(bump);
  ASSERT_TRUE(cold);
  const auto old_answer = answer->Capture();
  const auto old_bump = bump->Capture();
  const auto old_cold = cold->Capture();
  ASSERT_TRUE(old_answer);
  ASSERT_TRUE(old_bump);
  ASSERT_TRUE(old_cold);
  const auto initial = answer->Invoke({});
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial->AsInteger(), 15);
  const auto next =
      test::PrepareText(source,
                        "let added = 3; let cold = 200; const title = 'new'; let score = 100; "
                        "class Counter { static value: int = 400; } "
                        "export func answer(): int { "
                        "return score + Counter.value + (title == 'old' ? 1 : 1000) + 100; } "
                        "export func edit(): void { score = 1000; } "
                        "export func bump(): int { score += 100; return score; } "
                        "export func readCold(): int { return cold + added; }");
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  const auto compiled = (*compiler)->Compile(next->snapshot().program);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  auto prepared = dispatch.Prepare(1, compiled->image);
  ASSERT_TRUE(prepared) << prepared.error().message;
  // 候选已准备时，旧程序仍可继续写入；发布不能用 Prepare 时的值覆盖这些写入。
  ASSERT_TRUE(edit->Invoke({}));
  isolate.heap().Collect();
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  ASSERT_TRUE(source.Commit(*next));
  const auto current = answer->Invoke({});
  const auto previous = old_answer->Invoke({});
  ASSERT_TRUE(current);
  ASSERT_TRUE(previous);
  EXPECT_EQ(current->AsInteger(), 150);
  EXPECT_EQ(previous->AsInteger(), 50);
  const auto changed = old_bump->Invoke({});
  ASSERT_TRUE(changed);
  EXPECT_EQ(changed->AsInteger(), 43);
  const auto after_write = answer->Invoke({});
  ASSERT_TRUE(after_write);
  EXPECT_EQ(after_write->AsInteger(), 151);
  // 未初始化槽从旧函数首次读取也必须选用新初始化器。
  const auto lazy = old_cold->Invoke({});
  ASSERT_TRUE(lazy);
  EXPECT_EQ(lazy->AsInteger(), 200);
  const auto added = cold->Invoke({});
  ASSERT_TRUE(added);
  EXPECT_EQ(added->AsInteger(), 203);
}

TEST(JitCompilerTest, SharedGlobalInitializationRetriesFailuresAndDetectsCycles) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled =
      CompileText(**compiler,
                  "let divisor = 0; let answer = 84 / divisor; let cycle = readCycle(); "
                  "func readCycle(): int { return cycle; } "
                  "export func repair(): void { divisor = 2; } "
                  "export func read(operation: int): int { if (operation == 0) { return answer; } "
                  "return cycle; }");
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  const auto read = dispatch.FindEntry("main.vela", "read");
  const auto repair = dispatch.FindEntry("main.vela", "repair");
  ASSERT_TRUE(read);
  ASSERT_TRUE(repair);
  const std::array answer{runtime::Value::Integer(0)};
  EXPECT_FALSE(read->Invoke(answer));
  EXPECT_TRUE(isolate.native_control().CanPublish());
  ASSERT_TRUE(repair->Invoke({}));
  const auto retried = read->Invoke(answer);
  ASSERT_TRUE(retried);
  EXPECT_EQ(retried->AsInteger(), 42);
  const std::array cycle{runtime::Value::Integer(1)};
  EXPECT_FALSE(read->Invoke(cycle));
  EXPECT_FALSE(read->Invoke(cycle));
  EXPECT_TRUE(isolate.native_control().CanPublish());
}

class JitContractTest : public testing::TestWithParam<std::string_view> {};

TEST(JitCompilerTest, NominalTypesInHostCallsAndContainersSurviveLayoutVersions) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const std::string declarations =
      "interface Reader { read(): int; } "
      "class Box implements Reader { value: int; read(): int { return this.value; } } "
      "export func make(): Box { return Box(value: 40); } "
      "export func wrap(value: Box): Box[] { return [value]; } "
      "export func check(values: Box[], value: dynamic): bool { "
      "return values[0] == value && value is Box && value is Reader; } ";
  const auto first = test::PrepareText(
      source, declarations + "export func read(value: Box): int { return value.value; }");
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *first));
  const auto make = dispatch.FindEntry("main.vela", "make");
  const auto wrap = dispatch.FindEntry("main.vela", "wrap");
  const auto read = dispatch.FindEntry("main.vela", "read");
  const auto check = dispatch.FindEntry("main.vela", "check");
  ASSERT_TRUE(make);
  ASSERT_TRUE(wrap);
  ASSERT_TRUE(read);
  ASSERT_TRUE(check);
  const auto created = make->Invoke({});
  ASSERT_TRUE(created);
  const runtime::RootedValue object(isolate, *created);
  const std::array object_argument{object.value()};
  const auto wrapped = wrap->Invoke(object_argument);
  ASSERT_TRUE(wrapped);
  const runtime::RootedValue array(isolate, *wrapped);
  const auto next = test::PrepareText(
      source, "class Added { other: string = 'new'; } " + declarations +
                  "export func read(value: Box): int { return value.value + 2; }");
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *next));
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
  const auto result = read->Invoke(object_argument);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 42);
  const std::array checked{array.value(), object.value()};
  const auto matches = check->Invoke(checked);
  ASSERT_TRUE(matches);
  EXPECT_TRUE(matches->AsBoolean());
  const auto newer = make->Invoke({});
  ASSERT_TRUE(newer);
  const auto* previous_layout = runtime::Heap::ClassOf(*object.value().AsObject());
  const auto* current_layout = runtime::Heap::ClassOf(*newer->AsObject());
  EXPECT_EQ(previous_layout, current_layout);
  EXPECT_EQ(&previous_layout->nominal_type(), &current_layout->nominal_type());
}

TEST_P(JitContractTest, RejectsIncompatibleCallsAndSlotsWithoutChangingThePublishedRun) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first = test::PrepareText(source,
                                       "let value = 40; export func answer(input: int): int { "
                                       "return value + input; }");
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *first));
  const auto answer = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(answer);
  const auto previous = answer->Capture();
  ASSERT_TRUE(previous);
  const std::array arguments{runtime::Value::Integer(2)};
  const auto initialized = answer->Invoke(arguments);
  ASSERT_TRUE(initialized);
  EXPECT_EQ(initialized->AsInteger(), 42);
  const auto counts = isolate.layout_counts();
  const auto next = test::PrepareText(source, std::string(GetParam()));
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  ASSERT_TRUE(test::RequiresReload(**compiler, isolate, *next));
  EXPECT_EQ(dispatch.revision(), 1U);
  EXPECT_EQ(source.snapshot()->revision, 1U);
  EXPECT_EQ(isolate.layout_counts(), counts);
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
  const auto current = answer->Invoke(arguments);
  const auto captured = previous->Invoke(arguments);
  ASSERT_TRUE(current);
  ASSERT_TRUE(captured);
  EXPECT_EQ(current->AsInteger(), 42);
  EXPECT_EQ(captured->AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(
    IncompatibleChanges,
    JitContractTest,
    testing::Values(
        "let value = 100; export func answer(other: int): int { return value + other; }",
        "let value = 100; export func answer(input: double): int { return value + input.toInt(); }",
        "let value = 100; export func answer(input: int?): int { return value + (input ?? 0); }",
        "let value = 100; export func answer(input: int): double { return 1.5; }",
        "let value = 100; export async func answer(input: int): Future<int> { return value + "
        "input; }",
        "let value = 'new'; export func answer(input: int): int { return value.length + input; }",
        "let value: int? = null; export func answer(input: int): int { return (value ?? 0) + "
        "input; }",
        "const value = 100; export func answer(input: int): int { return value + input; }"));

struct NominalChange final {
  std::string_view before;
  std::string_view after;
};

class JitNominalContractTest : public testing::TestWithParam<NominalChange> {};

TEST_P(JitNominalContractTest, RejectsTypeContractChangesIncludingUninstantiatedDeclarations) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto change = GetParam();
  const auto first = test::PrepareText(
      source, std::string(change.before) + " export func answer(): int { return 42; }");
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *first));
  const auto counts = isolate.layout_counts();
  const auto next = test::PrepareText(
      source, std::string(change.after) + " export func answer(): int { return 0; }");
  if (!next) {
    EXPECT_EQ(next.error().status, SourceCandidateStatus::RequiresReload);
    EXPECT_NE(next.error().declaration, DeclarationId{});
  } else {
    ASSERT_TRUE(test::RequiresReload(**compiler, isolate, *next));
  }
  EXPECT_EQ(isolate.layout_counts(), counts);
  EXPECT_EQ(dispatch.revision(), 1U);
  const auto answer = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(answer);
  const auto result = answer->Invoke({});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(
    IncompatibleTypes,
    JitNominalContractTest,
    testing::Values(NominalChange{"class A {}", ""},
                    NominalChange{"interface A { read(): int; }", ""},
                    NominalChange{"class A {}", "interface A { read(): int; }"},
                    NominalChange{"class A {} class B {} class C extends A {}",
                                  "class A {} class B {} class C extends B {}"},
                    NominalChange{
                        "interface A { read(): int; } class C { read(): int { return 1; } }",
                        "interface A { read(): int; } class C implements A { "
                        "read(): int { return 1; } }"},
                    NominalChange{"enum A { first, second }", "enum A { second, first }"},
                    NominalChange{"enum A { first, second }", "enum A { first, second, third }"},
                    NominalChange{"func same<T>(value: T): T { return value; }",
                                  "func same<T, U>(value: T): T { return value; }"},
                    NominalChange{"class Box<T> { value: T; }", "class Box<T, U> { value: T; }"}));

TEST(JitCompilerTest, OldInterfaceCallsSelectCurrentMethodsByStableSelectors) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first =
      test::PrepareText(source,
                        "interface Reader { read(): int; other(): int; } "
                        "class Box implements Reader { "
                        "read(): int { return 40; } other(): int { return 1; } } "
                        "export func make(): Box { return Box(); } "
                        "export func call(value: Reader): int { return value.read(); }");
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *first));
  const auto make = dispatch.FindEntry("main.vela", "make");
  const auto call = dispatch.FindEntry("main.vela", "call");
  ASSERT_TRUE(make);
  ASSERT_TRUE(call);
  const auto old_call = call->Capture();
  ASSERT_TRUE(old_call);
  const auto created = make->Invoke({});
  ASSERT_TRUE(created);
  const runtime::RootedValue receiver(isolate, *created);
  const auto next =
      test::PrepareText(source,
                        "interface Reader { other(): int; read(): int; } "
                        "class Box implements Reader { "
                        "other(): int { return 100; } read(): int { return 42; } } "
                        "export func make(): Box { return Box(); } "
                        "export func call(value: Reader): int { return value.read() + 1; }");
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *next));
  isolate.heap().Collect();
  const std::array arguments{receiver.value()};
  const auto previous = old_call->Invoke(arguments);
  const auto current = call->Invoke(arguments);
  ASSERT_TRUE(previous);
  ASSERT_TRUE(current);
  EXPECT_EQ(previous->AsInteger(), 42);
  EXPECT_EQ(current->AsInteger(), 43);
}

class JitLanguageTest
    : public testing::TestWithParam<std::tuple<std::string_view, NativeCodeTier>> {};

TEST_P(JitLanguageTest, ExecutesNativeProductionPathsWithPreciseRoots) {
  const auto [source, tier] = GetParam();
  SCOPED_TRACE(source);
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, std::string(source), tier);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "main");
  ASSERT_TRUE(entry);
  const auto result = entry->Invoke({});
  ASSERT_TRUE(result) << ExecutionStatusName(result.error());
  const runtime::RootedValue root(isolate, *result);
  auto value = root.value();
  if (value.IsObject()) {
    auto* future = runtime::Heap::GetFuture(*value.AsObject());
    ASSERT_NE(future, nullptr);
    ASSERT_TRUE(isolate.scheduler().PumpUntil(*future, runtime::FuturePumpMode::WaitForHost));
    ASSERT_EQ(future->state(), runtime::FutureState::Succeeded);
    value = *future->value();
  }
  ASSERT_TRUE(value.IsInteger());
  EXPECT_EQ(value.AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(
    NativeSemantics,
    JitLanguageTest,
    testing::Combine(testing::Values(
                         R"(
func sum(count: int): int { if (count == 0) { return 0; } return count + sum(count - 1); }
export func main(): int { let value = 0; for (let i = 0; i < 6; i++) { value += i; }
  switch (value) { case 15: return sum(6) << 1; default: return 0; } }
)",
                         ::vela::compiler::test::kRecursiveStringGcSource,
                         ::vela::compiler::test::kGenericAsyncMethodsSource,
                         ::vela::compiler::test::kUnicodeConstructionSource,
                         ::vela::compiler::test::kMapLiteralSource,
                         ::vela::compiler::test::kMapLiteralAsyncSource,
                         ::vela::compiler::test::kMapLiteralInitializationSource,
                         R"(
func counter(value: int): () => int { return (): int => { value++; return value; }; }
export func main(): int { const next = counter(40); const values: (() => int)[] = [next];
  next(); return values[0](); }
)",
                         R"(
interface Reader { read(): int; }
class Base { stored: int = 40; read(): int { return this.stored; } }
class Child extends Base implements Reader { override read(): int { return super.read() + 2; } }
export func main(): int { const reader: Reader = Child(); const callback = reader.read;
  return callback(); }
)",
                         R"(
func same<T>(value: T): T { return value; }
class Box<T> { private stored: T; constructor(value: T) { this.stored = value; }
  read(): T { return this.stored; } }
type IntegerBox = Box<int>;
export func main(): int { return same<int>(IntegerBox(42).read()); }
)",
                         R"(
let count = 0;
func initial(): int { count++; return 40; }
const value = initial();
export func main(): int { const first = value; return first + value - 39 + count; }
)",
                         R"(
func divide(value: int, denominator: int): int { return value / denominator; }
export func main(): int { const result = divide(84, 2);
  try { const zero = 0; return 1 / zero; }
  catch (error: Error) { return error.code == 'DivisionByZero' ? result : 0; } }
)",
                         R"(
enum Shade { light, dark }
func choose(value: int?): int { return match (value) { null => 0, let present => present }; }
export func main(): int { const selected = Shade.dark;
  return match (selected) { Shade.light => 0, Shade.dark => choose(42) }; }
)",
                         R"(
export func main(): int { const values = [1, 2, 3]; values.push(4);
  const mapped = values.map((value: int): int => value * 2);
  return mapped.reduce((sum: int, value: int): int => sum + value, 0) + 22; }
)",
                         R"(
class Item { value: int; }
export func main(): int { const key = Item(value: 40); const values = Map<Item, string>();
  values.set(key, 'ab'); const text = values.get(key);
  if (text != null && !values.has(Item(value: 40))) { return key.value + text.length; }
  return 0; }
)",
                         R"(
export func main(): int { const bytes = ByteArray(length: 8); const data = ByteData.view(bytes);
  const write: (int,int) => void = data.setInt32; write(0, 42);
  return data.getInt32(0) == 42 && bytes[3] == 42 ? 42 : 0; }
)",
                         R"(
export func main(): int { const root: dynamic = Json.parse('{"items":[40,2]}');
  const values: dynamic[] = root.get('items'); return (values[0] as int) + (values[1] as int); }
)",
                         R"(
export func main(): int { const found = RegExp('(?<digits>[0-9]+)').firstMatch('😀42');
  if (found != null) { return int.parse(found.namedGroup('digits') ?? '0'); } return 0; }
)",
                         R"(
export async func main(): Future<int> {
  const values = await Future.wait([Future.value(20), Future.delayed(1, (): int => 22)]);
  return values[0] + values[1]; }
)",
                         R"(
class Failure { value: int; }
async func failed(): Future<int> { await sleep(1); throw Failure(value: 40); }
export async func main(): Future<int> { let value = 0;
  try { value = await failed(); } catch (error: Failure) { value = error.value; }
  finally { value += 2; } return value; }
)"),
                     testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized)));

std::binary_semaphore& StopSignal() {
  static std::binary_semaphore signal{0};
  return signal;
}

void Entered(runtime::NativeContext& context) noexcept {
  EXPECT_TRUE(context.isolate().native_control().HasHostBorrow());
  StopSignal().release();
}

void Finalized() noexcept {
  ADD_FAILURE() << "Stopping a run must not execute its finalizers";
}

std::int64_t Twice(std::int64_t value) noexcept {
  return value * 2;
}

std::string CopyText(runtime::NativeContext& context, std::string_view text) noexcept {
  EXPECT_TRUE(context.isolate().native_control().HasHostBorrow());
  context.isolate().heap().Collect();
  return std::string(text);
}

TEST(JitCompilerTest, TypedMapsKeepNullableValuesAndGrowthBuffersAcrossHostCollection) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    const auto compiled = CompileText(**compiler, R"(
external func copy(text: string): string;
export func main(): int {
  const values = Map<int, string?>();
  if (values.get(0) != null) { return -1; }
  for (let index = 0; index < 300; index++) { values.set(index, index.toString()); }
  values.set(17, null);
  values.set(18, '');
  const copied = copy('keep');
  if (!values.has(17) || values.get(17) != null || values.get(18) != '' ||
      values.get(-1) != null) { return -1; }
  let length = copied.length;
  for (let index = 0; index < 300; index++) {
    const text = values.get(index);
    if (text != null) { length += text.length; }
  }
  return length;
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&CopyText>("copy")));
    static_cast<void>(isolate.FreezeBindings());
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", "main");
    ASSERT_TRUE(entry);
    ASSERT_TRUE(test::ReturnsInteger(*entry, 790));
  }
}

TEST(JitCompilerTest, LargeCheckpointFramesKeepScalarValuesAndRootsAcrossHostCollection) {
  constexpr std::int64_t kValues = 384;
  std::string text =
      "external func copy(text: string): string = 'copy'; "
      "export func wide(input: int): int { const text = input.toString(); ";
  for (std::int64_t index = 0; index < kValues; ++index) {
    text += "const value" + std::to_string(index) + " = input + " + std::to_string(index) + "; ";
  }
  text += "let sum = copy(text).length; ";
  for (std::int64_t index = 0; index < kValues; ++index) {
    text += "sum += value" + std::to_string(index) + "; ";
  }
  text += "return sum; }";
  SourceSession source("main.vela");
  const auto prepared = test::PrepareText(source, text);
  ASSERT_TRUE(prepared);
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    for (const auto optimization : {NativeOptimization::None, NativeOptimization::Release}) {
      auto compiler = Compiler::Create();
      ASSERT_TRUE(compiler);
      const auto compiled = (*compiler)->Compile(prepared->snapshot().program,
                                                 {.optimization = optimization, .tier = tier});
      ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
      runtime::Isolate isolate(1);
      ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<CopyText>("copy")));
      static_cast<void>(isolate.FreezeBindings());
      ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
      const auto entry = isolate.native_dispatch().FindEntry("main.vela", "wide");
      ASSERT_TRUE(entry);
      constexpr auto kConstantSum = kValues * (kValues - 1) / 2;
      const std::array positive{runtime::Value::Integer(1)};
      const std::array negative{runtime::Value::Integer(-100)};
      ASSERT_TRUE(test::ReturnsInteger(*entry, kConstantSum + kValues + 1, positive));
      ASSERT_TRUE(test::ReturnsInteger(*entry, kConstantSum - (kValues * 100) + 4, negative));
      const std::array overflow{runtime::Value::Integer(std::numeric_limits<std::int64_t>::max())};
      const auto failed = entry->Invoke(overflow);
      ASSERT_FALSE(failed);
      EXPECT_EQ(failed.error(), ExecutionStatus::IntegerOverflow);
    }
  }
}

TEST(JitCompilerTest, BoundedPureLoopsPreserveRangeFailuresAndObservablePrefixes) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled = CompileText(**compiler, R"(
let size = 1000;
export func arithmetic(): int {
  const bound = size;
  let result = 0;
  for (let index = 0; index < bound; index++) { result += index; }
  return result;
}
export func sum(start: int, bound: int): int {
  let result = 0;
  for (let index = start; index < bound; index++) { result += index; }
  return result;
}
export func descending(start: int, bound: int): int {
  let result = 0;
  for (let index = start; index > bound; index--) { result += index; }
  return result;
}
export func prefix(initial: int, bound: int): int {
  let result = initial;
  try { for (let index = 0; index < bound; index++) { result += 1; } }
  catch (error: Error) { return result; }
  return result;
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    EXPECT_NE(compiled->llvm_ir.find(".bounded"), std::string::npos) << compiled->llvm_ir;
    runtime::Isolate isolate;
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    auto& dispatch = isolate.native_dispatch();
    const auto sum = dispatch.FindEntry("main.vela", "sum");
    const auto descending = dispatch.FindEntry("main.vela", "descending");
    const auto prefix = dispatch.FindEntry("main.vela", "prefix");
    const auto arithmetic = dispatch.FindEntry("main.vela", "arithmetic");
    ASSERT_TRUE(sum);
    ASSERT_TRUE(descending);
    ASSERT_TRUE(prefix);
    ASSERT_TRUE(arithmetic);
    ASSERT_TRUE(test::ReturnsInteger(*arithmetic, 499500));
    for (const std::int64_t count : {0, 1, 31, 1024, 1025, 4096}) {
      SCOPED_TRACE(count);
      const std::array ascending_arguments{
          runtime::Value::Integer(0),
          runtime::Value::Integer(count),
      };
      ASSERT_TRUE(test::ReturnsInteger(*sum, count * (count - 1) / 2, ascending_arguments));
      const std::array descending_arguments{
          runtime::Value::Integer(count),
          runtime::Value::Integer(0),
      };
      ASSERT_TRUE(test::ReturnsInteger(*descending, count * (count + 1) / 2, descending_arguments));
    }
    constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
    const std::array overflow{runtime::Value::Integer(kMaximum - 3), runtime::Value::Integer(32)};
    ASSERT_TRUE(test::ReturnsInteger(*prefix, kMaximum, overflow));
    const std::array empty{runtime::Value::Integer(kMaximum), runtime::Value::Integer(-1)};
    ASSERT_TRUE(test::ReturnsInteger(*sum, 0, empty));
  }
}

TEST(JitCompilerTest, IntegerReductionsCheckExtremaBeforeOppositeSignsCancel) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    const auto compiled = CompileText(**compiler, R"(
export func ascending(start: int, bound: int, initial: int): int {
  let result = initial;
  for (let index = start; index < bound; index++) { result += index; }
  return result;
}
export func descending(start: int, bound: int, initial: int): int {
  let result = initial;
  for (let index = start; index > bound; index--) { result += index; }
  return result;
}
export func subtractAscending(start: int, bound: int, initial: int): int {
  let result = initial;
  for (let index = start; index < bound; index++) { result -= index; }
  return result;
}
export func subtractDescending(start: int, bound: int, initial: int): int {
  let result = initial;
  for (let index = start; index > bound; index--) { result -= index; }
  return result;
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate;
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    constexpr auto kMinimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
    for (const auto& [name, start, bound, initial] : {
             std::tuple{"ascending", -3, 4, kMinimum + 2},
             std::tuple{"descending", 3, -4, kMaximum - 2},
             std::tuple{"subtractAscending", -3, 4, kMaximum - 2},
             std::tuple{"subtractDescending", 3, -4, kMinimum + 2},
         }) {
      SCOPED_TRACE(name);
      const auto entry = isolate.native_dispatch().FindEntry("main.vela", name);
      ASSERT_TRUE(entry);
      const std::array safe{runtime::Value::Integer(start), runtime::Value::Integer(bound),
                            runtime::Value::Integer(42)};
      ASSERT_TRUE(test::ReturnsInteger(*entry, 42, safe));
      const std::array arguments{runtime::Value::Integer(start), runtime::Value::Integer(bound),
                                 runtime::Value::Integer(initial)};
      const auto failed = entry->Invoke(arguments);
      ASSERT_FALSE(failed);
      EXPECT_EQ(failed.error(), ExecutionStatus::IntegerOverflow);
      const std::array empty{runtime::Value::Integer(bound), runtime::Value::Integer(start),
                             runtime::Value::Integer(initial)};
      ASSERT_TRUE(test::ReturnsInteger(*entry, initial, empty));
    }
    const auto ascending = isolate.native_dispatch().FindEntry("main.vela", "ascending");
    const auto descending = isolate.native_dispatch().FindEntry("main.vela", "descending");
    ASSERT_TRUE(ascending);
    ASSERT_TRUE(descending);
    const std::array upper{runtime::Value::Integer(kMaximum - 1), runtime::Value::Integer(kMaximum),
                           runtime::Value::Integer(-(kMaximum - 1))};
    ASSERT_TRUE(test::ReturnsInteger(*ascending, 0, upper));
    const std::array lower{runtime::Value::Integer(kMinimum + 1), runtime::Value::Integer(kMinimum),
                           runtime::Value::Integer(kMaximum)};
    ASSERT_TRUE(test::ReturnsInteger(*descending, 0, lower));
    const std::array wide_ascending{runtime::Value::Integer(-kMaximum),
                                    runtime::Value::Integer(kMaximum), runtime::Value::Integer(0)};
    const auto ascending_overflow = ascending->Invoke(wide_ascending);
    ASSERT_FALSE(ascending_overflow);
    EXPECT_EQ(ascending_overflow.error(), ExecutionStatus::IntegerOverflow);
    const std::array wide_descending{runtime::Value::Integer(kMaximum),
                                     runtime::Value::Integer(-kMaximum),
                                     runtime::Value::Integer(0)};
    const auto descending_overflow = descending->Invoke(wide_descending);
    ASSERT_FALSE(descending_overflow);
    EXPECT_EQ(descending_overflow.error(), ExecutionStatus::IntegerOverflow);
  }
}

TEST(JitCompilerTest, BoundedLoopsThroughInlinedCallsPreserveResultsAndOverflowPrefixes) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto compiled = CompileText(**compiler, R"(
func mix(value: int): int { return (value ^ 4660) & 65535; }
func increment(value: int): int { return value + 1; }
export func value(seed: int): int {
  let result = seed;
  for (let index = 0; index < 64; index++) { result ^= mix(index); }
  return result;
}
export func prefix(initial: int): int {
  let result = initial;
  try { for (let index = 0; index < 64; index++) { result = increment(result); } }
  catch (error: Error) { return result; }
  return result;
}
)",
                                    NativeCodeTier::Optimized);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  EXPECT_NE(compiled->llvm_ir.find(".bounded"), std::string::npos);
  runtime::Isolate isolate;
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  const auto value = isolate.native_dispatch().FindEntry("main.vela", "value");
  const auto prefix = isolate.native_dispatch().FindEntry("main.vela", "prefix");
  ASSERT_TRUE(value);
  ASSERT_TRUE(prefix);
  constexpr std::int64_t kSeed = 123;
  constexpr std::int64_t kIterations = 64;
  constexpr std::int64_t kMixKey = 4660;
  constexpr std::int64_t kMixMask = 65535;
  std::int64_t expected = kSeed;
  for (std::int64_t index = 0; index < kIterations; ++index) {
    expected ^= (index ^ kMixKey) & kMixMask;
  }
  const std::array seed{runtime::Value::Integer(kSeed)};
  ASSERT_TRUE(test::ReturnsInteger(*value, expected, seed));
  constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
  const std::array overflow{runtime::Value::Integer(kMaximum - 3)};
  ASSERT_TRUE(test::ReturnsInteger(*prefix, kMaximum, overflow));
}

TEST(JitCompilerTest, ChunkedPureLoopsPreserveMemoryAndEarlyExitPrefixes) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    const auto compiled = CompileText(**compiler, R"(
export func run(count: int, cutoff: int): int {
  const values = [0];
  let iterations = 0;
  for (let index = 0; index < count; index++) {
    values[0] = index;
    iterations++;
    if (index == cutoff) { return values[0] + iterations; }
  }
  return values[0] + iterations;
}
export func fail(count: int, cutoff: int): int {
  const values = [0];
  let iterations = 0;
  try {
    for (let index = 0; index < count; index++) {
      values[0] = index;
      iterations++;
      if (index == cutoff) { values[1] = index; }
    }
  } catch (error: Error) { return values[0] + iterations; }
  return -1;
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    EXPECT_NE(compiled->llvm_ir.find("poll.chunk"), std::string::npos);
    runtime::Isolate isolate;
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    const auto run = isolate.native_dispatch().FindEntry("main.vela", "run");
    const auto fail = isolate.native_dispatch().FindEntry("main.vela", "fail");
    ASSERT_TRUE(run);
    ASSERT_TRUE(fail);
    for (const std::int64_t count : {0, 1, 1024, 1025, 4096}) {
      const std::array arguments{runtime::Value::Integer(count), runtime::Value::Integer(-1)};
      ASSERT_TRUE(test::ReturnsInteger(*run, count == 0 ? 0 : (2 * count) - 1, arguments));
    }
    const std::array cutoff{runtime::Value::Integer(4096), runtime::Value::Integer(1025)};
    ASSERT_TRUE(test::ReturnsInteger(*run, 2051, cutoff));
    ASSERT_TRUE(test::ReturnsInteger(*fail, 2051, cutoff));
  }
}

TEST(JitCompilerTest, StableSortKeepsReferenceRunsAliveAcrossUnevenMerges) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled = CompileText(**compiler, R"(
external func copy(text: string): string;
class Item { key: int; order: int; text: string; }
export func main(count: int): int {
  const values: Item[] = [];
  for (let i = 0; i < count; i++) {
    values.push(Item(key: (count - i) & 3, order: i, text: i.toString()));
  }
  values.sort((left, right) => { copy(left.text); return left.key - right.key; });
  for (let i = 1; i < count; i++) {
    const previous = values[i - 1]; const current = values[i];
    if (previous.key > current.key ||
        (previous.key == current.key && previous.order > current.order)) { return 0; }
  }
  return values.length == count ? 42 : 0;
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<CopyText>("copy")));
    static_cast<void>(isolate.FreezeBindings());
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", "main");
    ASSERT_TRUE(entry);
    for (const auto count : {0, 1, 2, 3, 7, 16, 31, 32, 33, 129}) {
      SCOPED_TRACE(count);
      const std::array arguments{runtime::Value::Integer(count)};
      ASSERT_TRUE(test::ReturnsInteger(*entry, 42, arguments));
    }
  }
}

std::expected<void, runtime::NativeError> Reject() noexcept {
  return std::unexpected(runtime::NativeError{.message = "rejected"});
}

TEST(JitCompilerTest, FixedHostBindingsUseDirectCheckedAndManagedCallingConventions) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, R"(
external func twice(value: int): int;
external func copyText(text: string): string;
external func reject(): void;
export func main(): int {
  const copy = copyText;
  const text = copy(['4', '0'].join(''));
  try { reject(); return 0; }
  catch (error: Error) {
    if (error.code != 'NativeFailure' || error.message != 'rejected') { return 0; }
  }
  return twice(20) + text.length;
}
)");
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&Twice>("twice")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&CopyText>("copyText")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&Reject>("reject")));
  static_cast<void>(isolate.FreezeBindings());
  ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "main");
  ASSERT_TRUE(entry);
  ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
  EXPECT_FALSE(isolate.native_control().HasHostBorrow());
}

std::int64_t UnexpectedInitialization() noexcept {
  ADD_FAILURE() << "Load must not execute a user initializer";
  return 0;
}

TEST(JitCompilerTest, AddedModulesExposeTheirExportsWithoutRunningUserInitializers) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(
      runtime::NativeFunction::Bind<&UnexpectedInitialization>("initialized")));
  static_cast<void>(isolate.FreezeBindings());
  auto& dispatch = isolate.native_dispatch();
  const auto first = test::PrepareText(source, "export func answer(): int { return 40; }");
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *first));
  const auto original = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(original);
  const auto added = source.Prepare({
      .base_revision = 1,
      .changes =
          {
              {
                  .path = "added.vela",
                  .text = "external func initialized(): int; let unused = initialized(); "
                          "export func answer(): int { return 42; }",
              },
          },
      .added_roots = {"added.vela"},
  });
  ASSERT_TRUE(added) << DiagnosticsText(added.error().diagnostics);
  ASSERT_TRUE(test::PublishSource(**compiler, source, isolate, *added));
  const auto loaded = dispatch.FindEntry("added.vela", "answer");
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->Invoke({})->AsInteger(), 42);
  EXPECT_EQ(original->Invoke({})->AsInteger(), 40);
}

TEST(JitCompilerTest, CrossThreadStopExitsANonAllocatingLoopWithoutRunningFinally) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled = CompileText(**compiler, R"(
external func entered(): void;
external func finalized(): void;
export func main(): void { entered(); try { while (true) {} } finally { finalized(); } }
export func counted(): void {
  const values = [0];
  entered();
  try {
    for (let index = 0; index < 9223372036854775807; index++) { values[0] = index; }
  } finally { finalized(); }
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    for (const auto* name : {"main", "counted"}) {
      runtime::Isolate isolate;
      ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&Entered>("entered")));
      ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&Finalized>("finalized")));
      static_cast<void>(isolate.FreezeBindings());
      ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
      const auto entry = isolate.native_dispatch().FindEntry("main.vela", name);
      ASSERT_TRUE(entry);
      std::jthread request([&] {
        constexpr auto kEntryTimeout = std::chrono::seconds(10);
        const auto entered = StopSignal().try_acquire_for(kEntryTimeout);
        EXPECT_TRUE(entered);
        if (entered) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        isolate.native_control().RequestStop();
      });
      const auto result = entry->Invoke({});
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error(), ExecutionStatus::ExecutionStopped);
      EXPECT_FALSE(isolate.native_control().HasHostBorrow());
    }
  }
}

}  // namespace
}  // namespace vela::compiler::jit
