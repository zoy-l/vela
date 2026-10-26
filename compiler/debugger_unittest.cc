#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "vm/bytecode_image.h"
#include "vm/debugger.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

constexpr std::int64_t kExpectedAnswer = 42;

class CallbackDebugger final : public vm::Debugger {
 public:
  explicit CallbackDebugger(std::function<vm::DebugAction(const vm::DebugContext&)> callback)
      : callback_(std::move(callback)) {}
  vm::DebugAction BeforeInstruction(const vm::DebugContext& context) noexcept override {
    return callback_(context);
  }

 private:
  std::function<vm::DebugAction(const vm::DebugContext&)> callback_;
};

void CollectAndCheckFrame(const vm::DebugContext& context,
                          runtime::Isolate& isolate,
                          std::vector<std::string>& frames) {
  isolate.heap().Collect();
  const auto current = context.current_frame();
  frames.emplace_back(current.name());
  EXPECT_FALSE(current.ReadRegister(current.register_count()));
  EXPECT_FALSE(context.Frame(context.frame_count()));
}

void ExpectCallerLocation(const vm::DebugFrame& caller) {
  const auto location = caller.location();
  if (!location) {
    ADD_FAILURE() << "missing source location";
    return;
  }
  EXPECT_EQ(location->path, "main.vela");
  EXPECT_EQ(location->position.line, 6U);
}

void ExpectChildStack(const vm::DebugContext& context) {
  EXPECT_EQ(context.current_frame().instruction(), 0U);
  const auto caller = context.Frame(1);
  if (!caller) {
    ADD_FAILURE() << "missing caller";
    return;
  }
  EXPECT_EQ(caller->name(), "main");
  ExpectCallerLocation(*caller);
}

vm::DebugAction StopBeforeInitialize(const vm::DebugContext& context) {
  return context.current_frame().name() == "initialize" ? vm::DebugAction::Stop
                                                        : vm::DebugAction::Continue;
}

void ExpectNoInitializingGlobals(const vm::Program& program) {
  const auto* globals = program.globals();
  ASSERT_NE(globals, nullptr);
  EXPECT_TRUE(std::ranges::none_of(
      globals->states, [](auto state) -> bool { return state == vm::GlobalState::Initializing; }));
}

TEST(DebuggerTest, ContinuesThroughAwaitAndScheduledCallbacks) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
func transform(value: int): int { return value + 2; }
async func main(): Future<int> {
  await 0;
  return await Future.value(40).then(transform);
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  bool saw_transform = false;
  CallbackDebugger debugger([&](const vm::DebugContext& context) {
    isolate.heap().Collect();
    saw_transform = saw_transform || context.current_frame().name() == "transform";
    return vm::DebugAction::Continue;
  });
  const auto result =
      vm::VirtualMachine(isolate).Debug(compiled.program.FindFunction("main"), debugger);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
  EXPECT_TRUE(saw_transform);
}

TEST(DebuggerTest, RootsLiveRegistersDuringEveryPausedInstruction) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
func work(text: string): int {
  const values: string[] = [text, text + "!"];
  return values[1].length;
}


func main(): int {
  const text = ["a", "b"].join("");
  return work(text) + 39;
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  std::size_t instructions = 0;
  std::vector<std::string> frames;
  CallbackDebugger debugger([&](const vm::DebugContext& context) -> vm::DebugAction {
    ++instructions;
    CollectAndCheckFrame(context, isolate, frames);
    return vm::DebugAction::Continue;
  });
  const auto result =
      vm::VirtualMachine(isolate).Debug(compiled.program.FindFunction("main"), debugger);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), kExpectedAnswer);
  EXPECT_GT(instructions, 0U);
  EXPECT_NE(std::ranges::find(frames, "work"), frames.end());
}

TEST(DebuggerTest, JsonConversionAndDynamicCallsKeepOrdinaryFramesAndGcRoots) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
class Payload {
  public param value: dynamic;
  public toJson(): dynamic[] { return [this.value, true, null]; }
  public read(): int { return this.value; }
}
func main(): int {
  const object: dynamic = Payload(value: 42);
  const read = object.read;
  const text = Json.stringify(object);
  const values: dynamic[] = Json.parse(text);
  return read() == values[0] && text == '[42,true,null]' ? 42 : 0;
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  std::vector<std::string> frames;
  CallbackDebugger debugger([&](const vm::DebugContext& context) {
    CollectAndCheckFrame(context, isolate, frames);
    return vm::DebugAction::Continue;
  });
  const auto result =
      vm::VirtualMachine(isolate).Debug(compiled.program.FindFunction("main"), debugger);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), kExpectedAnswer);
  EXPECT_NE(std::ranges::find(frames, "Payload.toJson"), frames.end());
  EXPECT_NE(std::ranges::find(frames, "Payload.read"), frames.end());
}

TEST(DebuggerTest, ReportsTheCurrentInstructionAndTheCallersCallSite) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
func child(): int {
  return 42;
}
func main(): int {
  return child();
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  bool stopped_in_child = false;
  CallbackDebugger debugger([&](const vm::DebugContext& context) -> vm::DebugAction {
    if (context.current_frame().name() != "child") {
      return vm::DebugAction::Continue;
    }
    stopped_in_child = true;
    ExpectChildStack(context);
    return vm::DebugAction::Stop;
  });
  const auto result =
      vm::VirtualMachine(isolate).Debug(compiled.program.FindFunction("main"), debugger);
  EXPECT_EQ(result.status, vm::ExecutionStatus::DebuggerStopped);
  EXPECT_TRUE(stopped_in_child);
}

TEST(DebuggerTest, StoppingAnInitializerDetachesRootsAndResetsItsInitializingState) {
  runtime::Isolate isolate(1);
  std::ostringstream output;
  isolate.SetOutput(output);
  const auto compiled = CompileSource(R"(
func initialize(): int { print("once"); return 42; }
let value = initialize();
func main(): int { return value; }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  CallbackDebugger debugger(StopBeforeInitialize);
  vm::VirtualMachine machine(isolate);
  const auto stopped = machine.Debug(compiled.program.FindFunction("main"), debugger);
  EXPECT_EQ(stopped.status, vm::ExecutionStatus::DebuggerStopped);
  EXPECT_TRUE(output.str().empty());
  ExpectNoInitializingGlobals(compiled.program);
  isolate.heap().Collect();
  const auto resumed = machine.Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(resumed.Succeeded());
  EXPECT_EQ(resumed.value.AsInteger(), kExpectedAnswer);
  EXPECT_EQ(output.str(), "once\n");
}

TEST(DebuggerTest, PreservesExceptionAndFinallySemantics) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
class Failure { param value: int; }
func fail(): int { throw Failure(value: 42); }
func main(): int {
  try { return fail(); }
  catch (error: Failure) { return error.value; }
  finally { const text = ["allocated", "here"].join(" "); }
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  CallbackDebugger debugger([&](const vm::DebugContext& /*unused*/) -> vm::DebugAction {
    isolate.heap().Collect();
    return vm::DebugAction::Continue;
  });
  const auto result =
      vm::VirtualMachine(isolate).Debug(compiled.program.FindFunction("main"), debugger);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), kExpectedAnswer);
}

TEST(DebuggerTest, DebugsLoadedImages) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("func main(): int { return 42; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto saved = vm::SaveBytecode(compiled.program, isolate);
  ASSERT_TRUE(saved);
  const auto loaded = vm::LoadBytecode(*saved, isolate);
  ASSERT_TRUE(loaded);
  std::size_t instructions = 0;
  CallbackDebugger debugger([&](const vm::DebugContext& /*unused*/) -> vm::DebugAction {
    ++instructions;
    return vm::DebugAction::Continue;
  });
  const auto result = vm::VirtualMachine(isolate).Debug(loaded->FindFunction("main"), debugger);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), kExpectedAnswer);
  EXPECT_GT(instructions, 0U);
}

TEST(DebuggerTest, ValidatesEntriesBeforeCallingTheObserver) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("func main(): int { return 42; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  std::size_t instructions = 0;
  CallbackDebugger debugger([&](const vm::DebugContext& /*unused*/) -> vm::DebugAction {
    ++instructions;
    return vm::DebugAction::Continue;
  });
  runtime::Isolate other;
  EXPECT_EQ(vm::VirtualMachine(other).Debug(compiled.program.FindFunction("main"), debugger).status,
            vm::ExecutionStatus::InvalidIsolate);
  EXPECT_EQ(vm::VirtualMachine(isolate).Debug(vm::FunctionHandle(), debugger).status,
            vm::ExecutionStatus::InvalidEntry);
  EXPECT_EQ(instructions, 0U);
}

}  // namespace
}  // namespace vela::compiler
