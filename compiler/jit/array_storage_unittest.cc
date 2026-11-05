#include <array>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

TEST(JitArrayStorageTest, ReferenceLoadsPreserveNullabilityAcrossDynamicErasureAndCollection) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    const auto compiled = test::CompileText(**compiler, R"(
class Item { value: int; }
export func make(): Item[] { return [Item(value: 40), Item(value: 2)]; }
export func makeNullable(): Item?[] { return [null, Item(value: 42)]; }
export func sum(values: Item[]): int {
  const erased: dynamic = values;
  const typed: Item[] = erased;
  return typed[0].value + typed[1].value;
}
export func nullable(values: Item?[]): int {
  const erased: dynamic = values;
  const typed: Item?[] = erased;
  const first = typed[0];
  if (first != null) { return 0; }
  const second = typed[1];
  if (second == null) { return 0; }
  return second.value;
}
export func fail(values: Item[]): int { return values[2].value; }
)",
                                            tier);
    ASSERT_TRUE(compiled) << frontend::DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    auto instance = runtime::NativeInstance::Borrow(isolate.native_dispatch().current_state());
    const auto make = instance.FindFunction("make");
    const auto make_nullable = instance.FindFunction("makeNullable");
    const auto sum = instance.FindFunction("sum");
    const auto nullable = instance.FindFunction("nullable");
    const auto fail = instance.FindFunction("fail");
    if (!make || !make_nullable || !sum || !nullable || !fail) {
      FAIL() << "missing array storage entry";
    }
    const auto made = runtime::ExecuteNativeFunction(instance, *make);
    ASSERT_TRUE(made.Succeeded());
    const auto values_storage = runtime::HostRoot::Create(isolate, made.value);
    ASSERT_TRUE(values_storage);
    const auto& values = *values_storage;
    const auto maybe = runtime::ExecuteNativeFunction(instance, *make_nullable);
    ASSERT_TRUE(maybe.Succeeded());
    const auto optional_storage = runtime::HostRoot::Create(isolate, maybe.value);
    ASSERT_TRUE(optional_storage);
    const auto& optional = *optional_storage;
    isolate.heap().Collect();
    const std::array input{values.value()};
    const auto total = runtime::ExecuteNativeFunction(instance, *sum, input);
    ASSERT_TRUE(total.Succeeded());
    EXPECT_EQ(total.value.AsInteger(), 42);
    const std::array optional_input{optional.value()};
    const auto selected = runtime::ExecuteNativeFunction(instance, *nullable, optional_input);
    ASSERT_TRUE(selected.Succeeded());
    EXPECT_EQ(selected.value.AsInteger(), 42);
    const auto failed = runtime::ExecuteNativeFunction(instance, *fail, input);
    EXPECT_EQ(failed.status, ExecutionStatus::IndexOutOfBounds);
  }
}

}  // namespace
}  // namespace vela::compiler::jit
