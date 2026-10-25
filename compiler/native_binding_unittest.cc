#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/source_loader.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/rooted_value.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

using runtime::NativeContext;
using runtime::NativeError;

constexpr double kScaleFactor = 2.0;
constexpr unsigned char kInvalidUtf8Byte = 0xff;
constexpr std::int64_t kHostAnswer = 42;

auto Add(NativeContext& /*unused*/, std::int64_t left, std::int64_t right) noexcept
    -> std::int64_t {
  return left + right;
}

auto Scale(NativeContext& /*unused*/, double value) noexcept -> double {
  return value * kScaleFactor;
}

auto Invert(NativeContext& /*unused*/, bool value) noexcept -> bool {
  return !value;
}

auto CopyWithCollection(NativeContext& context, std::string_view text) noexcept -> std::string {
  context.isolate().heap().Collect();
  return std::string(text);
}

auto Fail(NativeContext& /*unused*/) noexcept -> std::expected<void, NativeError> {
  return std::unexpected(NativeError{.message = "host rejected request"});
}

auto InvalidString(NativeContext& /*unused*/) noexcept -> std::string {
  return {1, static_cast<char>(kInvalidUtf8Byte)};
}

auto Answer(NativeContext& /*unused*/) noexcept -> std::expected<std::int64_t, NativeError> {
  return kHostAnswer;
}

void Collect(NativeContext& context) noexcept {
  context.isolate().heap().Collect();
}

TEST(NativeBindingTest, DirectIndirectAndModuleCallsUseExactSignatures) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Add>("add")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Scale>("scale")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Invert>("invert")));
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import "host.vela" as host;
func main(): int {
  const operation: (int, int) => int = host.add;
  if (host.scale(1.5) == 3.0 && host.invert(false)) { return operation(40, 2); }
  return 0;
})",
      },
      {
          .path = "host.vela",
          .text = R"(
export external func add(left: int, right: int): int;
export external func scale(value: double): double;
export external func invert(value: bool): bool;
)",
      },
  });
  EXPECT_TRUE(frontend::CheckModules(sources, "main.vela").Succeeded());
  const auto compiled = CompileModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(NativeBindingTest, RejectsMissingBindingsAndAllSignatureMismatchesAtCompilation) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Scale>("scale")));
  const auto invalid = std::to_array<std::string_view>({
      "external func missing(): int;",
      "external func scale(value: int): double;",
      "external func scale(value: double): int;",
      "external func scale(): double;",
      "external func scale(value: double?): double;",
      "external func scale(value: double[]): double;",
      "external func scale<T>(value: T): T;",
  });
  for (const auto source : invalid) {
    const auto compiled = CompileSource(source, isolate);
    EXPECT_FALSE(compiled.Succeeded()) << source;
    EXPECT_FALSE(compiled.diagnostics.empty());
  }
}

TEST(NativeBindingTest, StringArgumentsSurviveHostCollectionAndReturnedValueCanBeRooted) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<CopyWithCollection>("copy")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Collect>("collect")));
  const auto compiled = CompileSource(R"(
external func copy(text: string): string;
external func collect(): void;
func main(): string { collect(); return copy("你好" + " host"); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  const runtime::RootedValue handle(isolate, result.value);
  isolate.heap().Collect();
  EXPECT_EQ(handle.value().AsString()->value(), "你好 host");
}

TEST(NativeBindingTest, HostFailureRetainsNativeDeclarationAndCallerTrace) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Fail>("fail")));
  const auto compiled =
      CompileSource("external func fail(): void;\nfunc main(): void { fail(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  EXPECT_EQ(result.status, vm::ExecutionStatus::NativeFailure);
  EXPECT_EQ(result.message, "host rejected request");
  ASSERT_EQ(result.stack_trace.size(), 2U);
  EXPECT_EQ(result.stack_trace.front().function, "fail");
  EXPECT_EQ(result.stack_trace.front().location.line, 1U);
  EXPECT_EQ(result.stack_trace.back().function, "main");
}

TEST(NativeBindingTest, AcceptsNativeEntryAndRejectsInvalidUtf8FromHost) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Answer>("main")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<InvalidString>("invalid")));
  const auto compiled =
      CompileSource("external func main(): int; external func invalid(): string;", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  vm::VirtualMachine machine(isolate);
  const auto result = machine.Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
  const auto failed = machine.Execute(compiled.program.FindFunction("invalid"));
  EXPECT_EQ(failed.status, vm::ExecutionStatus::NativeFailure);
  EXPECT_EQ(failed.message, "native function returned invalid UTF-8");
}

TEST(NativeBindingTest, HostFailureCanBeCaughtAsStandardError) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Fail>("fail")));
  const auto compiled = CompileSource(R"(
import "std:error";
external func fail(): void;
func main(): string {
  try { fail(); } catch (error: Error) { return error.message; }
  return "not caught";
})",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "host rejected request");
}

TEST(NativeBindingTest, RegistrationRejectsDuplicatesAndHostCallsKeepArgumentsRooted) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<CopyWithCollection>("copy")));
  const auto duplicate = isolate.RegisterNative(runtime::NativeFunction::Bind<Scale>("copy"));
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(), runtime::NativeRegistrationError::DuplicateName);
  const auto invalid = isolate.RegisterNative(runtime::NativeFunction::Bind<Scale>(""));
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), runtime::NativeRegistrationError::InvalidName);
  const auto binding = isolate.FindNative("copy");
  ASSERT_NE(binding, nullptr);
  auto* string = isolate.heap().AllocateString("retained argument");
  const std::array<runtime::Value, 1> input = {runtime::Value::String(string)};
  const auto result = binding->Invoke(isolate, input);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsString()->value(), "retained argument");
  const std::array<runtime::Value, 1> wrong = {runtime::Value::Integer(1)};
  const auto mismatched = binding->Invoke(isolate, wrong);
  ASSERT_FALSE(mismatched);
  EXPECT_EQ(mismatched.error().kind, runtime::NativeErrorKind::TypeMismatch);
}

}  // namespace
}  // namespace vela::compiler
