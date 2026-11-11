#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

class NativeGcBarrierTest
    : public testing::TestWithParam<
          std::tuple<std::tuple<NativeProgramMode, NativeCodeTier>, bool, runtime::GcPhase>> {};

TEST_P(NativeGcBarrierTest, GeneratedStoresRetainReferencesDuringMarkAndSweep) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const std::string source = R"(
class Node { value: int = 42; child: Node? = null; }
class Holder {
  child: Node? = null;
  items: Node?[] = [null];
  any: dynamic = null;
  text: string? = null;
}
let saved: Node? = null;
export func makeHolder(): Holder { return Holder(); }
export func makeNode(): Node { return Node(child: Node()); }
export func fieldStore(owner: Holder, value: Node): void { owner.child = value; }
export func arrayStore(owner: Holder, value: Node): void { owner.items[0] = value; }
export func dynamicStore(owner: Holder, value: dynamic): void { owner.any = value; }
export func stringStore(owner: Holder, value: string): void { owner.text = value; }
export func globalStore(owner: Holder, value: Node): void { saved = value; }
export func clear(): void { saved = null; }
export func readField(owner: Holder): int {
  const child = owner.child; return child == null ? 0 : child.value;
}
export func readArray(owner: Holder): int {
  const child = owner.items[0]; return child == null ? 0 : child.value;
}
export func readDynamic(owner: Holder): int { return (owner.any as Node).value; }
export func readString(owner: Holder): int {
  const text = owner.text; return text == null ? 0 : text.length;
}
export func readGlobal(owner: Holder): int {
  const child = saved; return child == null ? 0 : child.value;
}
)";
  runtime::Isolate isolate;
  std::unique_ptr<runtime::NativeInstance> closed_instance;
  runtime::HeapObject* state = nullptr;
  const auto& [configuration, marked, phase] = GetParam();
  const auto [mode, tier] = configuration;
  if (mode == NativeProgramMode::Closed) {
    const auto compiled = CompileSource(
        **compiler, source, {.diagnostics = {}, .compilation = {.tier = tier}, .reflection = {}});
    ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
    auto created = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
    ASSERT_TRUE(created);
    closed_instance = std::move(*created);
    state = closed_instance->state().front().AsObject();
  } else {
    const auto compiled = test::CompileText(**compiler, source, tier);
    ASSERT_TRUE(compiled) << frontend::DiagnosticsText(compiled.error());
    ASSERT_TRUE(test::PublishImage(isolate, compiled->image));
    state = &isolate.native_dispatch().current_state();
  }
  auto instance = runtime::NativeInstance::Borrow(*state);
  const auto make_holder = instance.FindFunction("makeHolder");
  const auto make_node = instance.FindFunction("makeNode");
  const auto clear = instance.FindFunction("clear");
  ASSERT_TRUE(make_holder);
  ASSERT_TRUE(make_node);
  ASSERT_TRUE(clear);
  auto& heap = isolate.heap();
  runtime::NativeExecution execution(instance);
  for (const auto& [store_name, read_name] : {
           std::pair{"fieldStore", "readField"},
           std::pair{"arrayStore", "readArray"},
           std::pair{"dynamicStore", "readDynamic"},
           std::pair{"stringStore", "readString"},
           std::pair{"globalStore", "readGlobal"},
       }) {
    SCOPED_TRACE(store_name);
    SCOPED_TRACE(static_cast<int>(phase));
    ASSERT_TRUE(runtime::ExecuteNativeFunction(instance, *clear).Succeeded());
    heap.Collect();
    const auto baseline = heap.object_count();
    const auto holder = runtime::ExecuteNativeFunction(instance, *make_holder);
    ASSERT_TRUE(holder.Succeeded());
    const runtime::Value owner = holder.value;
    const runtime::RootRange owner_root(heap, &owner, 1);
    const std::string_view store_kind = store_name;
    const bool string_store = store_kind == "stringStore";
    const bool dynamic_store = store_kind == "dynamicStore";
    const auto node = runtime::ExecuteNativeFunction(instance, *make_node);
    ASSERT_TRUE(node.Succeeded());
    const auto value =
        string_store ? runtime::Value::String(heap.AllocateString("retained")) : node.value;
    ASSERT_TRUE(value.IsReference());
    runtime::RootRange value_root;
    if (marked) {
      value_root.Attach(heap, &value, 1);
    }
    // 最后分配的根对象占用首次 Sweep 工作量，让未标记的 value 保持尚未清扫。
    const auto sentinel = runtime::Value::String(heap.AllocateString("sentinel"));
    const runtime::RootRange sentinel_root(heap, &sentinel, 1);
    if (phase != runtime::GcPhase::Idle) {
      heap.StartCollection();
      if (phase == runtime::GcPhase::Sweep) {
        while (heap.phase() == runtime::GcPhase::Mark) {
          heap.Step(1);
        }
      }
    }
    ASSERT_EQ(heap.phase(), phase);
    const auto store = instance.FindFunction(store_name);
    const auto read = instance.FindFunction(read_name);
    ASSERT_TRUE(store);
    ASSERT_TRUE(read);
    const auto entry = instance.image().functions.values()[*store].entry;
    const std::array arguments{
        runtime::EncodeNativeValue(owner),
        runtime::EncodeNativeValue(value),
    };
    runtime::NativeValue result;
    // 直接使用镜像入口，由 value_root 控制目标是否标记，避免宿主参数根掩盖屏障遗漏。
    ASSERT_EQ(entry(&execution, arguments.data(), arguments.size(), &result),
              static_cast<std::uint8_t>(ExecutionStatus::Returned));
    heap.FinishCollection();
    value_root.Detach();
    // 检查 value 的完整对象图存活；已标记但尚未扫描的目标也必须保留其子节点。
    const auto retained_count = baseline + (string_store ? 4 : 5);
    if (phase != runtime::GcPhase::Idle) {
      EXPECT_EQ(heap.object_count(), retained_count);
    }
    heap.Collect();
    EXPECT_EQ(heap.object_count(), retained_count);
    const std::array retained{owner};
    const auto loaded = runtime::ExecuteNativeFunction(instance, *read, retained);
    ASSERT_TRUE(loaded.Succeeded());
    EXPECT_EQ(loaded.value.AsInteger(), string_store ? 8 : 42);
    if (dynamic_store) {
      for (const auto empty : {runtime::Value::Object(nullptr), runtime::Value::String(nullptr)}) {
        heap.StartCollection();
        const std::array null_arguments{
            runtime::EncodeNativeValue(owner),
            runtime::EncodeNativeValue(empty),
        };
        ASSERT_EQ(entry(&execution, null_arguments.data(), null_arguments.size(), &result),
                  static_cast<std::uint8_t>(ExecutionStatus::Returned));
        heap.FinishCollection();
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    NativeModes,
    NativeGcBarrierTest,
    testing::Combine(
        testing::Values(std::tuple{NativeProgramMode::Closed, NativeCodeTier::Baseline},
                        std::tuple{NativeProgramMode::Reloadable, NativeCodeTier::Baseline},
                        std::tuple{NativeProgramMode::Reloadable, NativeCodeTier::Optimized}),
        testing::Bool(),
        testing::Values(runtime::GcPhase::Idle, runtime::GcPhase::Mark, runtime::GcPhase::Sweep)));

}  // namespace
}  // namespace vela::compiler::jit
