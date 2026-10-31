#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_dispatch.h"
#include "runtime/rooted_value.h"
#include "runtime/value.h"

namespace vela::compiler::jit {
namespace {

using frontend::DiagnosticsText;

struct JitCallableTest : public testing::TestWithParam<NativeCodeTier> {
  void SetUp() override {
    auto created = Compiler::Create();
    ASSERT_TRUE(created) << DiagnosticsText(created.error());
    compiler_ = std::move(*created);
  }

  std::expected<void, std::string> Publish(std::string text) {
    return test::UpdateSource(*compiler_, source_, isolate_, std::move(text), GetParam());
  }

  std::unique_ptr<Compiler> compiler_;
  SourceSession source_{"main.vela"};
  runtime::Isolate isolate_{1};
};

std::string FactorySource(int result) {
  const auto value = std::to_string(result);
  return "interface Reader { other(): int; read(): int; } "
         "class Box implements Reader { other(): int { return 9; } "
         "read(): int { return " +
         value +
         "; } } "
         "func answer(): int { return " +
         value +
         "; } "
         "export func topFactory(): () => int { return answer; } "
         "export func lambdaFactory(): () => int { return (): int => " +
         value +
         "; } "
         "export func boundFactory(value: Box): () => int { return value.read; } "
         "export func interfaceFactory(value: Reader): () => int { return value.read; } "
         "export func dynamicFactory(value: dynamic): dynamic { return value.read; } "
         "export func make(): Box { return Box(); } "
         "export func call(callback: () => int): int { return callback(); }";
}

TEST_P(JitCallableTest, OldFactoriesCaptureCurrentNamedVersionsAndTheirOwnLambdaVersions) {
  const auto initial = Publish(FactorySource(40));
  ASSERT_TRUE(initial) << initial.error();
  auto& dispatch = isolate_.native_dispatch();
  const auto make = dispatch.FindEntry("main.vela", "make");
  const auto call = dispatch.FindEntry("main.vela", "call");
  ASSERT_TRUE(make);
  ASSERT_TRUE(call);
  const auto created = make->Invoke({});
  ASSERT_TRUE(created);
  const runtime::RootedValue receiver(isolate_, *created);
  const std::array receiver_argument{receiver.value()};
  const std::array names{
      "topFactory", "lambdaFactory", "boundFactory", "interfaceFactory", "dynamicFactory",
  };
  std::vector<runtime::NativeVersionEntry> factories;
  std::vector<runtime::RootedValue> callbacks;
  for (std::size_t index = 0; index < names.size(); ++index) {
    const auto entry = dispatch.FindEntry("main.vela", names.at(index));
    ASSERT_TRUE(entry);
    const auto captured = entry->Capture();
    ASSERT_TRUE(captured);
    factories.push_back(*captured);
    const auto arguments = index < 2 ? std::span<const runtime::Value>{}
                                     : std::span<const runtime::Value>(receiver_argument);
    const auto callback = entry->Invoke(arguments);
    ASSERT_TRUE(callback);
    callbacks.emplace_back(isolate_, *callback);
  }
  const auto updated = Publish("func added(): double { return 1.5; } " + FactorySource(42));
  ASSERT_TRUE(updated) << updated.error();
  isolate_.heap().Collect();
  for (std::size_t index = 0; index < names.size(); ++index) {
    SCOPED_TRACE(names.at(index));
    const std::array previous_argument{callbacks.at(index).value()};
    const auto previous = call->Invoke(previous_argument);
    ASSERT_TRUE(previous);
    EXPECT_EQ(previous->AsInteger(), 40);
    const auto arguments = index < 2 ? std::span<const runtime::Value>{}
                                     : std::span<const runtime::Value>(receiver_argument);
    const auto created_callback = factories.at(index).Invoke(arguments);
    ASSERT_TRUE(created_callback);
    const runtime::RootedValue callback(isolate_, *created_callback);
    const std::array current_argument{callback.value()};
    const auto selected = call->Invoke(current_argument);
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->AsInteger(), index == 1 ? 40 : 42);
  }
  EXPECT_EQ(compiler_->resource_counts().batches, 2U);
  factories.clear();
  callbacks.clear();
  isolate_.heap().Collect();
  EXPECT_EQ(compiler_->resource_counts().batches, 1U);
}

TEST_P(JitCallableTest, DynamicArrayCallbacksUseTheCapturedStateAndFunctionIndexTogether) {
  const std::string declarations =
      "export func factory(): (int) => int { return increment; } "
      "export func map(callback: (int) => int): int { "
      "const values: dynamic = [40]; const result: dynamic = values.map(callback); "
      "return result[0] as int; } ";
  const auto initial =
      Publish(declarations + "func increment(value: int): int { return value + 1; }");
  ASSERT_TRUE(initial) << initial.error();
  auto& dispatch = isolate_.native_dispatch();
  const auto factory = dispatch.FindEntry("main.vela", "factory");
  const auto map = dispatch.FindEntry("main.vela", "map");
  ASSERT_TRUE(factory);
  ASSERT_TRUE(map);
  const auto old_factory = factory->Capture();
  const auto old_map = map->Capture();
  ASSERT_TRUE(old_factory);
  ASSERT_TRUE(old_map);
  const auto created = factory->Invoke({});
  ASSERT_TRUE(created);
  const runtime::RootedValue previous_callback(isolate_, *created);
  const auto updated = Publish("func added(): string { return 'new'; } " + declarations +
                               "func increment(value: int): int { return value + 2; }");
  ASSERT_TRUE(updated) << updated.error();
  const auto created_after_update = old_factory->Invoke({});
  ASSERT_TRUE(created_after_update);
  const runtime::RootedValue current_callback(isolate_, *created_after_update);
  isolate_.heap().Collect();
  const std::array previous_argument{previous_callback.value()};
  const std::array current_argument{current_callback.value()};
  const auto previous = map->Invoke(previous_argument);
  const auto current = old_map->Invoke(current_argument);
  ASSERT_TRUE(previous);
  ASSERT_TRUE(current);
  EXPECT_EQ(previous->AsInteger(), 41);
  EXPECT_EQ(current->AsInteger(), 42);
}

TEST_P(JitCallableTest, FixedStandardDeclarationsRemainAvailableToRetainedOldCode) {
  const auto initial = Publish("export func answer(): int { return int.parse('42'); }");
  ASSERT_TRUE(initial) << initial.error();
  auto& dispatch = isolate_.native_dispatch();
  const auto answer = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(answer);
  const auto previous = answer->Capture();
  ASSERT_TRUE(previous);
  const auto updated = Publish("export func answer(): int { return 44; }");
  ASSERT_TRUE(updated) << updated.error();
  const auto old_result = previous->Invoke({});
  const auto current_result = answer->Invoke({});
  ASSERT_TRUE(old_result);
  ASSERT_TRUE(current_result);
  EXPECT_EQ(old_result->AsInteger(), 42);
  EXPECT_EQ(current_result->AsInteger(), 44);
}

TEST_P(JitCallableTest, GenericMethodClosuresRetainConcreteVersionsAfterCallsDisappear) {
  const auto declarations = [](int increment) {
    return "interface Mapper<T> { read<U extends int>(value: U): int; } "
           "class Owner<T> implements Mapper<T> { offset: int; "
           "read<U extends int>(value: U): int { return this.offset + value + " +
           std::to_string(increment) +
           "; } } "
           "export func call(callback: (int) => int): int { return callback(20); } ";
  };
  const auto initial = Publish(declarations(0) + R"(
export func boundFactory(): (int) => int { return Owner<string>(offset: 20).read<int>; }
export func interfaceFactory(): (int) => int {
  const owner: Mapper<string> = Owner<string>(offset: 20);
  return owner.read<int>;
}
)");
  ASSERT_TRUE(initial) << initial.error();
  auto& dispatch = isolate_.native_dispatch();
  const auto call = dispatch.FindEntry("main.vela", "call");
  ASSERT_TRUE(call);
  const std::array names{"boundFactory", "interfaceFactory"};
  std::vector<runtime::NativeVersionEntry> factories;
  std::vector<runtime::RootedValue> callbacks;
  for (const auto* name : names) {
    const auto factory = dispatch.FindEntry("main.vela", name);
    ASSERT_TRUE(factory);
    const auto captured = factory->Capture();
    const auto callback = factory->Invoke({});
    ASSERT_TRUE(captured);
    ASSERT_TRUE(callback);
    factories.push_back(*captured);
    callbacks.emplace_back(isolate_, *callback);
  }
  const auto retained = dispatch.RetainedDeclarations();
  const auto updated = test::UpdateSource(*compiler_, source_, isolate_, declarations(2) + R"(
export func boundFactory(): (int) => int { return (value: int): int => 0; }
export func interfaceFactory(): (int) => int { return (value: int): int => 0; }
)",
                                          GetParam(), retained);
  ASSERT_TRUE(updated) << updated.error();
  isolate_.heap().Collect();
  for (std::size_t index = 0; index < factories.size(); ++index) {
    SCOPED_TRACE(names.at(index));
    const std::array previous_argument{callbacks.at(index).value()};
    ASSERT_TRUE(test::ReturnsInteger(*call, 40, previous_argument));
    const auto created = factories.at(index).Invoke({});
    ASSERT_TRUE(created);
    const runtime::RootedValue callback(isolate_, *created);
    const std::array current_argument{callback.value()};
    ASSERT_TRUE(test::ReturnsInteger(*call, 42, current_argument));
  }
}

INSTANTIATE_TEST_SUITE_P(Tiers,
                         JitCallableTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::compiler::jit
