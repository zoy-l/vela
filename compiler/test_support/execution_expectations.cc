#include "compiler/test_support/execution_expectations.h"

#include <cstdint>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/test_support/checker_test_support.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_instance.h"

namespace vela::compiler::test {
namespace {

void ExpectRejectedDiagnostics(std::span<const frontend::Diagnostic> diagnostics) {
  EXPECT_TRUE(frontend::HasErrors(diagnostics)) << frontend::DiagnosticsText(diagnostics);
}

}  // namespace

void ExpectAnswer(runtime::NativeInstance& instance) {
  const auto result = ExecuteFutureInteger(instance);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kExpectedAnswer);
}

void ExpectInteger(runtime::NativeInstance& instance,
                   std::int64_t expected,
                   std::string_view entry) {
  const auto result = Execute(instance, entry);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), expected);
}

void ExpectRunAnswer(std::string_view source) {
  runtime::Isolate isolate(1);
  ExpectRunAnswer(source, isolate);
}

void ExpectRunAnswer(std::string_view source, runtime::Isolate& isolate) {
  const auto result = RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kExpectedAnswer);
}

void ExpectFutureAnswer(std::string_view source, std::int64_t expected) {
  runtime::Isolate isolate;
  const auto entry = CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = ExecuteFutureInteger(**entry);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, expected);
}

void ExpectRestoredAnswer(std::string_view source) {
  runtime::Isolate isolate(1);
  const auto loaded = ReinstantiateSource(source, isolate);
  ASSERT_TRUE(loaded) << loaded.error();
  ExpectAnswer(**loaded);
}

void ExpectStableAnswer(runtime::NativeInstance& instance) {
  auto& heap = instance.isolate().heap();
  const auto retained = heap.object_count();
  const auto result = ExecuteInteger(instance);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kExpectedAnswer);
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), retained);
}

void ExpectReleasedAnswer(std::string_view source) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  const auto baseline = heap.allocated_bytes();
  {
    const auto compiled = CompileSource(source, isolate);
    ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
    ExpectAnswer(*compiled.instance);
  }
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), baseline);
}

void ExpectStableAnswer(std::string_view source, runtime::Isolate& isolate) {
  auto& heap = isolate.heap();
  const auto retained = heap.object_count();
  ExpectRunAnswer(source, isolate);
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), retained);
}

void ExpectRejectedSource(std::string_view source, runtime::Isolate& isolate) {
  const auto compiled = CompileSource(source, isolate);
  EXPECT_FALSE(compiled.Succeeded());
  ExpectRejectedDiagnostics(compiled.diagnostics);
}

void ExpectRejectedSource(std::string_view source) {
  runtime::Isolate isolate;
  ExpectRejectedSource(source, isolate);
}

void ExpectRejectedCheck(std::string_view source) {
  const auto checked = frontend::CheckTestSource(source);
  EXPECT_FALSE(checked.Succeeded());
  ExpectRejectedDiagnostics(checked.diagnostics);
}

}  // namespace vela::compiler::test
