#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

TEST(JitArrayStorageTest, DirectFieldReductionsPreserveEmptyInputsAndOverflowWritePrefixes) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    const auto compiled = test::CompileText(**compiler, R"(
let initialized = 0;
func initial(): int { initialized++; return -1; }
class Counter {
  value: int = 0;
  add(value: int): int { this.value = value; return this.value; }
  risky(value: int): int { this.value = value; return value + 1; }
  bump(): int { this.value += 1; return this.value; }
}
class Child extends Counter { padding: int = 7; }
const object = Counter(value: initial());
const child = Child();
export func field(values: int[]): int {
  let sum = 0;
  for (let i = 0; i < values.length; i++) { object.value = values[i]; sum += object.value; }
  return sum;
}
export func method(values: int[]): int {
  let sum = 0;
  for (let i = 0; i < values.length; i++) { sum += object.add(values[i]); }
  return sum;
}
export func inherited(values: int[]): int {
  let result = 0;
  for (let i = 0; i < values.length; i++) { result = child.bump(); }
  return result;
}
export func risky(values: int[]): int {
  let sum = 0;
  for (let i = 0; i < values.length; i++) { sum += object.risky(values[i]); }
  return sum;
}
export func make(count: int, negative: bool): int[] {
  const values: int[] = [];
  for (let i = 0; i < count; i++) { values.push(negative ? -i : i); }
  return values;
}
export func overflow(): int[] { return [9223372036854775807, 1, -9223372036854775807]; }
export func maximum(): int[] { return [9223372036854775807]; }
export func reset(): void { object.value = -1; }
export func read(): int { return object.value; }
export func effects(): int { return initialized; }
)",
                                            tier);
    ASSERT_TRUE(compiled) << frontend::DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    auto& dispatch = isolate.native_dispatch();
    const auto make = dispatch.FindEntry("main.vela", "make");
    const auto field = dispatch.FindEntry("main.vela", "field");
    const auto method = dispatch.FindEntry("main.vela", "method");
    const auto inherited = dispatch.FindEntry("main.vela", "inherited");
    const auto risky = dispatch.FindEntry("main.vela", "risky");
    const auto reset = dispatch.FindEntry("main.vela", "reset");
    const auto read = dispatch.FindEntry("main.vela", "read");
    const auto effects = dispatch.FindEntry("main.vela", "effects");
    ASSERT_TRUE(make && field && method && inherited && risky && reset && read && effects);
    for (const std::int64_t count : {0, 1, 127, 128, 129, 511, 512, 513, 1024, 1025, 4096}) {
      for (const auto negative : {false, true}) {
        SCOPED_TRACE(count);
        SCOPED_TRACE(negative);
        const auto values = make->Invoke(
            std::array{runtime::Value::Integer(count), runtime::Value::Boolean(negative)});
        ASSERT_TRUE(values);
        const auto rooted = runtime::HostRoot::Create(isolate, *values);
        ASSERT_TRUE(rooted);
        isolate.heap().Collect();
        const std::array arguments{rooted->value()};
        const auto sign = negative ? -1 : 1;
        const auto expected = sign * count * (count - 1) / 2;
        if (count == 0) {
          ASSERT_TRUE(test::ReturnsInteger(*field, 0, arguments));
          ASSERT_TRUE(test::ReturnsInteger(*method, 0, arguments));
          ASSERT_TRUE(test::ReturnsInteger(*effects, 0));
        } else {
          for (const auto* entry : {&*field, &*method}) {
            ASSERT_TRUE(reset->Invoke({}));
            ASSERT_TRUE(test::ReturnsInteger(*entry, expected, arguments));
            ASSERT_TRUE(test::ReturnsInteger(*read, sign * (count - 1)));
          }
        }
      }
    }
    const auto input =
        make->Invoke(std::array{runtime::Value::Integer(128), runtime::Value::Boolean(false)});
    ASSERT_TRUE(input);
    const auto inherited_values = runtime::HostRoot::Create(isolate, *input);
    ASSERT_TRUE(inherited_values);
    const std::array inherited_arguments{inherited_values->value()};
    ASSERT_TRUE(test::ReturnsInteger(*inherited, 128, inherited_arguments));
    ASSERT_TRUE(test::ReturnsInteger(*inherited, 256, inherited_arguments));
    for (const auto* factory : {"overflow", "maximum"}) {
      const auto entry = dispatch.FindEntry("main.vela", factory);
      ASSERT_TRUE(entry);
      const auto values = entry->Invoke({});
      ASSERT_TRUE(values);
      const auto rooted = runtime::HostRoot::Create(isolate, *values);
      ASSERT_TRUE(rooted);
      const std::array arguments{rooted->value()};
      if (std::string_view(factory) == "overflow") {
        for (const auto* kernel : {&*field, &*method}) {
          ASSERT_TRUE(reset->Invoke({}));
          const auto failed = kernel->Invoke(arguments);
          ASSERT_FALSE(failed);
          EXPECT_EQ(failed.error(), ExecutionStatus::IntegerOverflow);
          ASSERT_TRUE(test::ReturnsInteger(*read, 1));
        }
      } else {
        const auto failed = risky->Invoke(arguments);
        ASSERT_FALSE(failed);
        EXPECT_EQ(failed.error(), ExecutionStatus::IntegerOverflow);
        ASSERT_TRUE(test::ReturnsInteger(*read, std::numeric_limits<std::int64_t>::max()));
      }
    }
  }
}

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
