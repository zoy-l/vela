#include <filesystem>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "base/native_program_mode.h"
#include "base/path.h"
#include "compiler/bindgen/fixture/bindingfixture_bindings.h"
#include "compiler/bindgen/fixture_paths.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

class GeneratedNativeClassTest : public testing::TestWithParam<NativeCodeTier> {};

TEST_P(GeneratedNativeClassTest, ExecutesGeneratedDualProductsThroughProductionImportsAndAbi) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler.has_value());
  const auto path = PathText(Utf8Path(binding_fixture::kModulePath).parent_path() / "entry.vela");
  constexpr auto kSource = R"(
import 'bindingfixture';
func callback(): (int) => int { return Counter(initial: 30).Collect; }
export func run(): int {
  const counter = Counter(initial: 1);
  if (counter.add(2) != 3 || counter.Collect(4) != 7) { return -1; }
  counter.value = 40;
  if (counter.value != 40 || counter.Checked(false) != 40) { return -2; }
  if (counter.Text('hello') != '40:hello' || counter.Equal(null)) { return -3; }
  const shared = Counter.Shared(40);
  if (shared == null || !counter.Equal(shared) || Counter.Empty() != null) { return -4; }
  const dynamicCounter: dynamic = counter;
  if (dynamicCounter.add(2) != 42 || !(dynamicCounter is Counter)) { return -5; }
  const bound = callback();
  if (bound(12) != 42) { return -6; }
  const values = [40, 2];
  const echoed = Counter.Echo(values);
  echoed[0] += echoed[1];
  if (values[0] != 42) { return -8; }
  try { counter.Checked(true); } catch (error: Error) { return counter.value; }
  return -7;
}
export async func retained(): Future<int> {
  const counter = Counter(initial: 40);
  await Future.microtask((): int => 0);
  return counter.Collect(2);
}
)";
  const frontend::SourceInput entry{.path = path, .text = kSource};
  const auto compiled = CompileParsed(**compiler, frontend::LoadProgram(path, {&entry, 1}),
                                      {.diagnostics = {}, .compilation = {.tier = GetParam()}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate isolate(1);
  ASSERT_TRUE(bindings::module_bindingfixture::Register(isolate));
  auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_TRUE(instance.has_value()) << instance.error().message;
  const auto function = (*instance)->FindFunction("run");
  if (!function) {
    FAIL() << "expected exported function run";
  }
  runtime::NativeExecution execution(**instance);
  const auto returned = runtime::InvokeNativeFunction(execution, *function);
  ASSERT_TRUE(returned.has_value()) << static_cast<unsigned>(returned.error());
  EXPECT_EQ(returned->AsInteger(), 42);
  const auto async_function = (*instance)->FindFunction("retained");
  if (!async_function) {
    FAIL() << "expected exported function retained";
  }
  const auto future = runtime::InvokeNativeFunction(execution, *async_function);
  ASSERT_TRUE(future.has_value());
  const auto pending_storage = runtime::HostRoot::Create(isolate, *future);
  ASSERT_TRUE(pending_storage.has_value());
  const auto& pending = *pending_storage;
  isolate.heap().Collect();
  EXPECT_TRUE(test::CompletesWithInteger(isolate, pending.value(), 42));
  isolate.heap().Collect();
}

TEST_P(GeneratedNativeClassTest, HotReloadKeepsCanonicalHostObjectsAndImmutableBindings) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler.has_value());
  const auto path = PathText(Utf8Path(binding_fixture::kModulePath).parent_path() / "reload.vela");
  SourceSession source(path);
  const auto declarations = frontend::LoadProgram(binding_fixture::kModulePath);
  ASSERT_FALSE(frontend::HasErrors(declarations.diagnostics));
  const auto& native_source = declarations.program.sources.front();
  runtime::Isolate isolate(1);
  ASSERT_TRUE(bindings::module_bindingfixture::Register(isolate));
  static_cast<void>(isolate.FreezeBindings());
  const auto update = [&](std::string text) {
    auto candidate = source.Prepare({
        .base_revision = source.snapshot()->revision,
        .changes =
            {
                {.path = path, .text = std::move(text)},
                {.path = native_source.path, .text = std::string(native_source.text)},
            },
        .added_roots = {},
    });
    if (!candidate) {
      return testing::AssertionFailure()
             << frontend::DiagnosticsText(candidate.error().diagnostics);
    }
    return test::PublishSource(**compiler, source, isolate, *candidate, GetParam());
  };
  const std::string initial = R"(
import 'bindingfixture';
let counter = Counter(initial: 40);
export func run(): int { return counter.Collect(1); }
export func object(): Counter { return counter; }
)";
  ASSERT_TRUE(update(initial));
  auto& dispatch = isolate.native_dispatch();
  auto first = dispatch.FindEntry(path, "run");
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(test::ReturnsInteger(*first, 41));
  const auto get_object = dispatch.FindEntry(path, "object");
  ASSERT_TRUE(get_object.has_value());
  const auto previous = get_object->Invoke({});
  ASSERT_TRUE(previous.has_value());
  auto changed = initial;
  const auto call = changed.find("counter.Collect(1);");
  ASSERT_NE(call, std::string::npos);
  changed.replace(call, std::string("counter.Collect(1);").size(), "counter.Collect(1) + 1;");
  ASSERT_TRUE(update(std::move(changed)));
  const auto second = dispatch.FindEntry(path, "run");
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(test::ReturnsInteger(*second, 43));
  const auto next_object = dispatch.FindEntry(path, "object");
  ASSERT_TRUE(next_object.has_value());
  const auto next = next_object->Invoke({});
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(previous->AsObject(), next->AsObject());
}

INSTANTIATE_TEST_SUITE_P(Tiers,
                         GeneratedNativeClassTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::compiler::jit
