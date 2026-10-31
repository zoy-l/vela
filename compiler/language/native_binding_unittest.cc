#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "compiler/bytecode/compiler.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/checker_test_support.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_context.h"
#include "runtime/native_function.h"
#include "runtime/register_storage.h"
#include "runtime/rooted_value.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/bytecode_image.h"
#include "vm/debugger.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

using runtime::NativeContext;
using runtime::NativeError;

runtime::FutureCompletionEndpoint*& PendingFetchCompleter() {
  static runtime::FutureCompletionEndpoint* completer = nullptr;
  return completer;
}

constexpr double kScaleFactor = 2.0;
constexpr unsigned char kInvalidUtf8Byte = 0xff;
constexpr std::int64_t kHostAnswer = 42;
constexpr std::int64_t kNativeFutureValue = 42;

std::int64_t Add(NativeContext& /*unused*/, std::int64_t left, std::int64_t right) noexcept {
  return left + right;
}

double Scale(NativeContext& /*unused*/, double value) noexcept {
  return value * kScaleFactor;
}

bool Invert(NativeContext& /*unused*/, bool value) noexcept {
  return !value;
}

std::int64_t PlainAdd(std::int64_t left, std::int64_t right) noexcept {
  return left + right;
}
double PlainScale(double value) noexcept {
  return value * kScaleFactor;
}
bool PlainInvert(bool value) noexcept {
  return !value;
}
void PlainNoop() noexcept {}
std::expected<std::int64_t, NativeError> PlainAnswer() noexcept {
  return kHostAnswer;
}
std::expected<void, NativeError> PlainReject() noexcept {
  return std::unexpected(NativeError{.message = "plain host rejected request"});
}

TEST(NativeBindingTest, ContextFreeFunctionsShareSignatureValidationCodecsAndFailures) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<PlainAdd>("add")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<PlainScale>("scale")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<PlainInvert>("invert")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<PlainNoop>("noop")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<PlainAnswer>("answer")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<PlainReject>("reject")));
  const auto result = test::RunInteger(R"(
external func add(left: int, right: int): int;
external func scale(value: double): double;
external func invert(value: bool): bool;
external func noop(): void;
external func answer(): int;
external func reject(): void;
func main(): int {
  noop();
  if (scale(21.0) != 42.0 || !invert(false) || invert(true)) { return 0; }
  try { reject(); return 0; }
  catch (error: Error) {
    if (error.code != 'NativeFailure' || error.message != 'plain host rejected request') { return 0; }
  }
  return add(answer(), 0);
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kHostAnswer);
}

struct Reentry final {
  vm::VirtualMachine* machine = nullptr;
  vm::FunctionHandle nested;
};

Reentry*& ActiveReentry() {
  static Reentry* active = nullptr;
  return active;
}

std::expected<std::string, NativeError> ReenterWithBorrowedString(NativeContext& context,
                                                                  std::string_view text) noexcept {
  auto& active = *ActiveReentry();
  const auto result = active.machine->Execute(active.nested);
  if (!result.Succeeded()) {
    return std::unexpected(NativeError{.message = "nested execution failed"});
  }
  context.isolate().heap().Collect();
  return std::string(text);
}

class NativeDebugger final : public vm::Debugger {
 public:
  NativeDebugger(runtime::Isolate& isolate, vm::DebugAction action)
      : isolate_(isolate), action_(action) {}

  vm::DebugAction BeforeInstruction(const vm::DebugContext& context) noexcept override {
    const auto frame = context.current_frame();
    if (frame.name() != "copy") {
      return vm::DebugAction::Continue;
    }
    saw_native_ = true;
    const auto value = frame.ReadRegister(0);
    isolate_.heap().Collect();
    if (value && value->IsString()) {
      argument_ = value->AsString()->value();
    }
    const auto caller = context.Frame(1);
    if (caller) {
      caller_name_ = caller->name();
    }
    return action_;
  }

  [[nodiscard]] bool saw_native() const noexcept { return saw_native_; }
  [[nodiscard]] std::string_view argument() const noexcept { return argument_; }
  [[nodiscard]] std::string_view caller_name() const noexcept { return caller_name_; }

 private:
  bool saw_native_ = false;
  std::string argument_;
  std::string caller_name_;
  runtime::Isolate& isolate_;
  vm::DebugAction action_;
};

std::string CopyWithCollection(NativeContext& context, std::string_view text) noexcept {
  context.isolate().heap().Collect();
  return std::string(text);
}

std::expected<void, NativeError> Fail(NativeContext& /*unused*/) noexcept {
  return std::unexpected(NativeError{.message = "host rejected request"});
}

std::string InvalidString(NativeContext& /*unused*/) noexcept {
  return {1, static_cast<char>(kInvalidUtf8Byte)};
}

std::expected<std::int64_t, NativeError> Answer(NativeContext& /*unused*/) noexcept {
  return kHostAnswer;
}

void Collect(NativeContext& context) noexcept {
  context.isolate().heap().Collect();
}

runtime::Future Fetch(NativeContext& context) noexcept {
  auto completer = context.CreateCompleter();
  const auto future = completer.future();
  *PendingFetchCompleter() = std::move(completer).Detach();
  return future;
}

runtime::Future EchoFuture(NativeContext& /*unused*/, runtime::Future future) noexcept {
  return future;
}

runtime::Future CompletedWithoutScheduler(NativeContext& /*unused*/) noexcept {
  return runtime::Future::Value(runtime::Value::Integer(kNativeFutureValue));
}

runtime::Future WrongFuturePayload(NativeContext& /*unused*/) noexcept {
  return runtime::Future::Value(runtime::Value::Boolean(true));
}

runtime::Future WrongFutureError(NativeContext& /*unused*/) noexcept {
  return runtime::Future::Error(runtime::Value::Integer(kNativeFutureValue));
}

std::expected<void, runtime::NativeRegistrationError> BindFutureContract(
    runtime::Isolate& isolate) {
  auto bindings = std::to_array<runtime::NativeFunction>({
      runtime::NativeFunction::Bind<CompletedWithoutScheduler>("ready"),
      runtime::NativeFunction::Bind<WrongFuturePayload>("wrong"),
      runtime::NativeFunction::Bind<WrongFutureError>("wrongError"),
  });
  for (auto& binding : bindings) {
    const auto registered = isolate.RegisterNative(std::move(binding));
    if (!registered) {
      return registered;
    }
  }
  return {};
}

constexpr std::string_view kNativeFutureContract = R"(
external func ready(): Future<int>;
external func wrong(): Future<int>;
external func wrongError(): Future<int>;
async func main(): Future<int> {
  try { await wrong(); return 0; }
  catch (error: Error) {
    if (error.code != 'TypeError') { return 0; }
  }
  try { await wrongError(); return 0; }
  catch (error: Error) {
    if (error.code != 'TypeError') { return 0; }
  }
  return await ready();
}
)";

TEST(NativeBindingTest, NativeFutureIsBoundAndItsPayloadIsCheckedBeforeScriptConsumption) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(BindFutureContract(isolate));
  const auto compiled = test::CompileCleanSource(kNativeFutureContract, isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteFutureInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kNativeFutureValue);
}

TEST(NativeBindingTest, NativeFuturePayloadChecksSurviveFreshIsolateImageLoading) {
  std::vector<std::byte> image;
  {
    runtime::Isolate isolate;
    ASSERT_TRUE(BindFutureContract(isolate));
    const auto compiled = CompileSource(kNativeFutureContract, isolate);
    ASSERT_TRUE(compiled.Succeeded());
    const auto saved = vm::SaveBytecode(compiled.program, isolate);
    ASSERT_TRUE(saved);
    image = *saved;
  }
  runtime::Isolate isolate(1);
  ASSERT_TRUE(BindFutureContract(isolate));
  const auto loaded = vm::LoadBytecode(image, isolate);
  ASSERT_TRUE(loaded) << loaded.error().message;
  const auto result = test::ExecuteFutureInteger(*loaded, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kNativeFutureValue);
}

runtime::Future WaitForCancellation(NativeContext& context,
                                    runtime::CancellationToken token) noexcept {
  auto completer = context.CreateCompleter();
  const auto future = completer.future();
  token.Observe([endpoint = std::move(completer).Detach()] {
    const auto completion = endpoint.Complete(runtime::Value::Integer(42));
    EXPECT_TRUE(completion.has_value());
  });
  return future;
}

TEST(NativeBindingTest, CancellationTokenReachesNativeProducerWithoutCancellingFutureItself) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<WaitForCancellation>("waitCancel")));
  const auto compiled = CompileSource(R"(
external func waitCancel(token: CancellationToken): Future<int>;
async func main(): Future<int> {
  const token = CancellationToken();
  const pending = waitCancel(token);
  token.cancel();
  return await pending;
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = test::ExecuteFutureInteger(compiled.program, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kNativeFutureValue);
}

std::optional<runtime::Future> StartNativeFetch(runtime::Isolate& isolate) {
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<Fetch>("fetch"))) {
    return std::nullopt;
  }
  const auto compiled = CompileSource(R"(
external func fetch(): Future<int>;
async func main(): Future<int> { return await fetch(); }
)",
                                      isolate);
  if (!compiled.Succeeded()) {
    return std::nullopt;
  }
  const auto entry = compiled.program.FindFunction("main");
  if (!entry.IsValid()) {
    return std::nullopt;
  }
  return vm::VirtualMachine(isolate).ExecuteFuture(compiled.program, entry.index());
}

bool CompleteNativeFuture(runtime::FutureCompletionEndpoint& completer) {
  std::atomic_bool succeeded = false;
  std::thread host([&completer, &succeeded] {
    succeeded = completer.Complete(runtime::Value::Integer(kNativeFutureValue)).has_value();
  });
  host.join();
  return succeeded;
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
  EXPECT_TRUE(frontend::CheckTestModules(sources, "main.vela").Succeeded());
  const auto compiled = test::CompileCleanModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
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

TEST(NativeBindingTest, DirectNativeCallsRetainOnlyCallerRegisterCapacity) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Add>("add")));
  const auto compiled = CompileSource(R"(
external func add(left: int, right: int): int;
func main(): int { return add(add(10, 20), 12); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto entry = compiled.program.FindFunction("main");
  vm::VirtualMachine machine(isolate);
  const auto result = machine.Execute(entry);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), kHostAnswer);
  EXPECT_EQ(machine.register_bytes(),
            compiled.program.functions().subspan(entry.index()).front().register_count *
                runtime::kRegisterSlotBytes);
}

TEST(NativeBindingTest, ReentryOnSameVmPreservesBorrowedArgumentsAndOuterRegisters) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<ReenterWithBorrowedString>("reenter")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Collect>("collect")));
  const auto compiled = CompileSource(R"(
external func reenter(text: string): string;
external func collect(): void;
func nested(): int {
  const values = [10, 20, 12];
  const text = 'nested' + ' allocations';
  collect();
  return values[0] + values[1] + values[2] + text.length;
}
func main(): string {
  const text = 'outer' + ' argument';
  const copied = reenter(text);
  collect();
  return text + ':' + copied;
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  vm::VirtualMachine machine(isolate);
  Reentry reentry{.machine = &machine, .nested = compiled.program.FindFunction("nested")};
  ActiveReentry() = &reentry;
  const auto result = machine.Execute(compiled.program.FindFunction("main"));
  ActiveReentry() = nullptr;
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsString()->value(), "outer argument:outer argument");
}

TEST(NativeBindingTest, DynamicNativeCallsUseValidatedParametersAcrossCollection) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<CopyWithCollection>("copy")));
  const auto entry = test::CompileEntry(R"(
external func copy(text: string): string;
func main(): string {
  const callable: dynamic = copy;
  return callable('borrowed' + ' value') as string;
}
)",
                                        isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = vm::VirtualMachine(isolate).Execute(*entry);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsString()->value(), "borrowed value");
}

TEST(NativeBindingTest, DebuggerSeesNativeDeclarationAndCanCollectBorrowedArguments) {
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<CopyWithCollection>("copy")));
  const auto entry = test::CompileEntry(R"(
external func copy(text: string): string;
func main(): string { return copy('first' + ' second'); }
)",
                                        isolate);
  ASSERT_TRUE(entry) << entry.error();
  NativeDebugger debugger(isolate, vm::DebugAction::Continue);
  const auto result = vm::VirtualMachine(isolate).Debug(*entry, debugger);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_TRUE(debugger.saw_native());
  EXPECT_EQ(debugger.argument(), "first second");
  EXPECT_EQ(debugger.caller_name(), "main");
  EXPECT_EQ(result.value.AsString()->value(), "first second");

  NativeDebugger stopping(isolate, vm::DebugAction::Stop);
  const auto stopped = vm::VirtualMachine(isolate).Debug(*entry, stopping);
  EXPECT_EQ(stopped.status, vm::ExecutionStatus::DebuggerStopped);
  EXPECT_TRUE(stopping.saw_native());
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
  std::array<runtime::RegisterPayload, 1> payloads{};
  std::array<runtime::ValueKind, 1> kinds{};
  const runtime::RegisterView input(payloads, kinds);
  input.front().SetString(string);
  runtime::RootRange roots;
  roots.Attach(isolate.heap(), input);
  const auto result = binding->Invoke(isolate, input);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsString()->value(), "retained argument");
  input.front().SetInteger(1);
  const auto mismatched = binding->Invoke(isolate, input);
  ASSERT_FALSE(mismatched);
  EXPECT_EQ(mismatched.error().kind, runtime::NativeErrorKind::TypeMismatch);
}

TEST(NativeBindingTest, NativeFutureResumesAwaitingAsyncExecution) {
  runtime::Isolate isolate;
  runtime::FutureCompletionEndpoint completer;
  PendingFetchCompleter() = &completer;
  auto root = StartNativeFetch(isolate);
  ASSERT_TRUE(root.has_value());
  auto root_future = root.value_or(runtime::Future{});
  ASSERT_EQ(root_future.state(), runtime::FutureState::Pending);
  ASSERT_TRUE(CompleteNativeFuture(completer));
  ASSERT_TRUE(isolate.scheduler().PumpUntil(root_future));
  ASSERT_EQ(root_future.state(), runtime::FutureState::Succeeded);
  ASSERT_EQ(root_future.value().value_or(runtime::Value::Null()).AsInteger(), kNativeFutureValue);
  PendingFetchCompleter() = nullptr;
}

TEST(NativeBindingTest, ForeignProgramCallablesCannotReuseLocalFunctionIndices) {
  runtime::Isolate isolate(1);
  const auto producer = CompileSource("func main(): () => int { return (): int => 7; }", isolate);
  ASSERT_TRUE(producer.Succeeded());
  const auto produced = vm::VirtualMachine(isolate).Execute(producer.program.FindFunction("main"));
  ASSERT_TRUE(produced.Succeeded());
  const runtime::RootedValue callable(isolate, produced.value);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Fetch>("fetch")));
  const auto consumer = CompileSource(R"(external func fetch(): Future<dynamic>;
async func main(): Future<int> {
  const callable: dynamic = await fetch();
  const typed: () => int = callable;
  let caught = 0;
  try { callable(); } catch (error: Error) { caught += 1; }
  try { typed(); } catch (error: Error) { caught += 1; }
  return caught == 2 ? 42 : 0;
})",
                                      isolate);
  ASSERT_TRUE(consumer.Succeeded());
  runtime::FutureCompletionEndpoint completer;
  PendingFetchCompleter() = &completer;
  auto result = vm::VirtualMachine(isolate).ExecuteFuture(
      consumer.program, consumer.program.FindFunction("main").index());
  PendingFetchCompleter() = nullptr;
  ASSERT_TRUE(completer.Complete(callable.value()));
  ASSERT_TRUE(isolate.scheduler().PumpUntil(result));
  ASSERT_EQ(result.state(), runtime::FutureState::Succeeded);
  EXPECT_EQ(result.value().value_or(runtime::Value::Null()).AsInteger(), kNativeFutureValue);
}

TEST(NativeBindingTest, NativeFutureParametersUseHeapBackedCodec) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<EchoFuture>("echo")));
  const auto compiled = CompileSource(R"(
external func echo(input: Future<int>): Future<int>;
async func main(): Future<int> { return await echo(Future.value(9)); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto entry = compiled.program.FindFunction("main");
  ASSERT_TRUE(entry.IsValid());
  auto root = vm::VirtualMachine(isolate).ExecuteFuture(compiled.program, entry.index());
  ASSERT_TRUE(isolate.scheduler().PumpUntil(root));
  ASSERT_EQ(root.state(), runtime::FutureState::Succeeded);
  EXPECT_EQ(root.value().value_or(runtime::Value::Null()).AsInteger(), 9);
}

}  // namespace
}  // namespace vela::compiler
