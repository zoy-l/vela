#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/callable.h"
#include "base/execution_status.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_debugger.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/strings/string.h"

namespace vela::compiler {
namespace {

class CallbackDebugger final : public runtime::NativeDebugger {
 public:
  CallbackDebugger(runtime::Isolate& isolate,
                   std::function<void(const runtime::NativeDebugContext&)> callback)
      : isolate_(&isolate), callback_(std::move(callback)) {
    isolate_->native_control().SetDebugger(this);
  }
  ~CallbackDebugger() override { isolate_->native_control().SetDebugger(nullptr); }
  void AtSafepoint(const runtime::NativeDebugContext& context) noexcept override { callback_(context); }

 private:
  runtime::Isolate* isolate_;
  std::function<void(const runtime::NativeDebugContext&)> callback_;
};

void ExpectAnswer(runtime::NativeInstance& instance) {
  const auto result = test::ExecuteFutureInteger(instance);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

void CollectAndCheckFrame(const runtime::NativeDebugContext& context,
                          runtime::Isolate& isolate,
                          std::vector<std::string>& frames) {
  const auto& frame = context.top_frame();
  std::vector<std::pair<std::size_t, std::string>> strings;
  for (std::size_t index = 0; index < frame.register_count(); ++index) {
    const auto value = frame.ReadRegister(index);
    if (value && value->IsString()) {
      strings.emplace_back(index, value->AsString()->value());
    }
  }
  isolate.heap().Collect();
  frames.emplace_back(frame.name());
  EXPECT_FALSE(frame.ReadRegister(frame.register_count()));
  for (const auto& [index, text] : strings) {
    const auto value = frame.ReadRegister(index);
    ASSERT_TRUE(value && value->IsString());
    EXPECT_EQ(value->AsString()->value(), text);
  }
}

TEST(DebuggerTest, ContinuesThroughAwaitAndScheduledCallbacks) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileDebugSource(R"(
func transform(value: int): int { return value + 2; }
async func main(): Future<int> {
  await 0;
  return await Future.value(40).then(transform);
}
)", isolate);
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  bool saw_transform = false;
  CallbackDebugger debugger(isolate, [&](const runtime::NativeDebugContext& context) {
    isolate.heap().Collect();
    saw_transform = saw_transform || context.top_frame().name() == "transform";
  });
  ExpectAnswer(*compiled.instance);
  EXPECT_TRUE(saw_transform);
}

TEST(DebuggerTest, RootsLiveValuesDuringEveryPausedInstruction) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileDebugSource(R"(
func work(text: string): int {
  const values: string[] = [text, text + "!"];
  return values[1].length;
}
func main(): int {
  const text = ["a", "b"].join("");
  return work(text) + 39;
}
)", isolate);
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  std::vector<std::string> frames;
  CallbackDebugger debugger(isolate, [&](const runtime::NativeDebugContext& context) {
    CollectAndCheckFrame(context, isolate, frames);
  });
  ExpectAnswer(*compiled.instance);
  EXPECT_NE(std::ranges::find(frames, "work"), frames.end());
}

TEST(DebuggerTest, JsonConversionAndDynamicCallsKeepOrdinaryFramesAndGcRoots) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileDebugSource(R"(
class Payload {
  value: dynamic;
  toJson(): dynamic[] { return [this.value, true, null]; }
  read(): int { return this.value; }
}
func main(): int {
  const object: dynamic = Payload(value: 42);
  const read = object.read;
  const text = Json.stringify(object);
  const values: dynamic[] = Json.parse(text);
  return read() == values[0] && text == '[42,true,null]' ? 42 : 0;
}
)", isolate);
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  std::vector<std::string> frames;
  CallbackDebugger debugger(isolate, [&](const runtime::NativeDebugContext& context) {
    CollectAndCheckFrame(context, isolate, frames);
  });
  ExpectAnswer(*compiled.instance);
  EXPECT_NE(std::ranges::find(frames, "Payload.toJson"), frames.end());
  EXPECT_NE(std::ranges::find(frames, "Payload.read"), frames.end());
}

TEST(DebuggerTest, ReportsTheCurrentInstructionAndTheCallersCallSite) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileDebugSource(R"(
func child(): int {
  return 42;
}
func main(): int {
  return child();
}
)", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  bool stopped_in_child = false;
  CallbackDebugger debugger(isolate, [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() != "child") {
      return;
    }
    stopped_in_child = true;
    EXPECT_EQ(context.top_frame().instruction(), 0U);
    const auto frames = context.frames();
    ASSERT_EQ(frames.size(), 2U);
    const auto& caller = frames.subspan(1).front();
    EXPECT_EQ(caller.name(), "main");
    EXPECT_EQ(caller.source_path(), "main.vela");
    ASSERT_TRUE(caller.location());
    EXPECT_EQ(caller.location()->line, 6U);
    context.RequestStop();
  });
  EXPECT_EQ(test::Execute(*compiled.instance).status, ExecutionStatus::ExecutionStopped);
  EXPECT_TRUE(stopped_in_child);
}

TEST(DebuggerTest, StoppingAnInitializerResetsItsStateAndTerminatesTheRun) {
  runtime::Isolate isolate(1);
  std::ostringstream output;
  isolate.SetOutput(output);
  const auto compiled = test::CompileDebugSource(R"(
func initialize(): int { print("once"); return 42; }
let value = initialize();
func main(): int { return value; }
)", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  {
    CallbackDebugger debugger(isolate, [](const runtime::NativeDebugContext& context) {
      if (context.top_frame().name() == "initialize") { context.RequestStop(); }
    });
    EXPECT_EQ(test::Execute(*compiled.instance).status, ExecutionStatus::ExecutionStopped);
  }
  EXPECT_TRUE(output.str().empty());
  const auto& state = runtime::Heap::NativeProgram(*compiled.instance->state().front().AsObject());
  const auto states = std::span(state.global_states,
                               static_cast<std::size_t>(compiled.instance->image().global_initializers.size));
  EXPECT_TRUE(std::ranges::none_of(states, [](auto value) {
    return value == runtime::NativeProgramState::GlobalState::Initializing;
  }));
  isolate.heap().Collect();
  EXPECT_EQ(test::Execute(*compiled.instance).status, ExecutionStatus::ExecutionStopped);
  runtime::Isolate restarted(1);
  restarted.SetOutput(output);
  auto instance = runtime::NativeInstance::Instantiate(restarted, state.image_owner);
  ASSERT_TRUE(instance);
  ExpectAnswer(**instance);
  EXPECT_EQ(output.str(), "once\n");
}

TEST(DebuggerTest, PreservesExceptionAndFinallySemantics) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileDebugSource(R"(
class Failure { value: int; }
func fail(): int { throw Failure(value: 42); }
func main(): int {
  try { return fail(); }
  catch (error: Failure) { return error.value; }
  finally { const text = ["allocated", "here"].join(" "); }
}
)", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  CallbackDebugger debugger(isolate, [&](const runtime::NativeDebugContext&) { isolate.heap().Collect(); });
  ExpectAnswer(*compiled.instance);
}

TEST(DebuggerTest, DebugsAnImageAfterItsCompilerAndOriginalInstanceAreDestroyed) {
  runtime::Isolate producer;
  auto compiled = test::CompileDebugSource("func main(): int { return 42; }", producer);
  ASSERT_TRUE(compiled.Succeeded());
  runtime::Isolate isolate(1);
  auto instance = runtime::NativeInstance::Instantiate(
      isolate, runtime::Heap::NativeProgram(*compiled.instance->state().front().AsObject()).image_owner);
  ASSERT_TRUE(instance);
  compiled.instance.reset();
  producer.heap().Collect();
  std::size_t instructions = 0;
  CallbackDebugger debugger(isolate, [&](const runtime::NativeDebugContext&) { ++instructions; });
  ExpectAnswer(**instance);
  EXPECT_GT(instructions, 0U);
}

TEST(DebuggerTest, ValidatesEntriesBeforeCallingTheObserver) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileDebugSource(
      "func main(): int { return 42; } func required(value: int): int { return value; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  std::size_t instructions = 0;
  CallbackDebugger debugger(isolate, [&](const runtime::NativeDebugContext&) { ++instructions; });
  EXPECT_FALSE(compiled.instance->FindFunction("missing"));
  EXPECT_EQ(runtime::ExecuteNativeFunction(*compiled.instance, kInvalidFunctionTarget).status,
            ExecutionStatus::InvalidEntry);
  const auto required = compiled.instance->FindFunction("required");
  ASSERT_TRUE(required);
  EXPECT_EQ(runtime::ExecuteNativeFunction(*compiled.instance, *required).status, ExecutionStatus::TypeError);
  EXPECT_EQ(instructions, 0U);
}

}  // namespace
}  // namespace vela::compiler
