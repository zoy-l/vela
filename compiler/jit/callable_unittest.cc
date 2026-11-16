#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

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

void CollectCallbackInputs(runtime::NativeContext& context) noexcept {
  context.isolate().heap().Collect();
}

TEST_P(JitCallableTest, HostCallbacksKeepExplicitAndImplicitInputsAcrossCollection) {
  ASSERT_TRUE(
      isolate_.RegisterNative(runtime::NativeFunction::Bind<CollectCallbackInputs>("collect")));
  const auto published = Publish(R"(
external func collect(): void;
class Box {
  value: int = 40;
  add(value: int): int { collect(); return this.value + value; }
}
export func make(): Box { return Box(); }
export func read(value: Box): int { collect(); return value.value + 2; }
export func bound(): (int) => int { return Box().add; }
export func parser(): (string, int?) => int { return int.parse; }
export func text(): string { return 42.toString(); }
)");
  ASSERT_TRUE(published) << published.error();
  auto& dispatch = isolate_.native_dispatch();
  const auto make = dispatch.FindEntry("main.vela", "make");
  const auto read = dispatch.FindEntry("main.vela", "read");
  const auto bound = dispatch.FindEntry("main.vela", "bound");
  const auto parser = dispatch.FindEntry("main.vela", "parser");
  const auto text = dispatch.FindEntry("main.vela", "text");
  ASSERT_TRUE(make);
  ASSERT_TRUE(read);
  ASSERT_TRUE(bound);
  ASSERT_TRUE(parser);
  ASSERT_TRUE(text);
  const auto receiver = make->Invoke({});
  ASSERT_TRUE(receiver);
  const std::array explicit_arguments{*receiver};
  const auto explicit_result = read->Invoke(explicit_arguments);
  ASSERT_TRUE(explicit_result);
  EXPECT_EQ(explicit_result->AsInteger(), 42);

  const auto target = bound->Resolve();
  ASSERT_TRUE(target);
  auto instance = runtime::NativeInstance::Borrow(*target->state);
  runtime::NativeExecution execution(instance);
  const auto callback = bound->Invoke({});
  ASSERT_TRUE(callback);
  const std::array implicit_arguments{runtime::Value::Integer(2)};
  const auto implicit_result =
      runtime::InvokeNativeCallable(execution, *callback, implicit_arguments);
  ASSERT_TRUE(implicit_result);
  EXPECT_EQ(implicit_result->AsInteger(), 42);

  const auto parse = parser->Invoke({});
  ASSERT_TRUE(parse);
  const auto parser_root = runtime::HostRoot::Create(isolate_, *parse);
  ASSERT_TRUE(parser_root);
  const auto source = text->Invoke({});
  ASSERT_TRUE(source);
  const std::array optional_arguments{*source};
  const auto optional_result =
      runtime::InvokeNativeCallable(execution, parser_root->value(), optional_arguments);
  ASSERT_TRUE(optional_result);
  EXPECT_EQ(optional_result->AsInteger(), 42);
  const std::array invalid_arguments{runtime::Value::Boolean(true)};
  const auto rejected =
      runtime::InvokeNativeCallable(execution, parser_root->value(), invalid_arguments);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), ExecutionStatus::InvalidCast);
}

TEST_P(JitCallableTest, ArrayCallbackArgumentsKeepCanonicalLayoutsAndNullability) {
  ASSERT_TRUE(
      isolate_.RegisterNative(runtime::NativeFunction::Bind<CollectCallbackInputs>("collect")));
  const auto published = Publish(R"(
external func collect(): void;
export func integers(): int[] { return [40, 2]; }
export func decimals(): double[] { return [40.0, 2.0]; }
export func empty(): int[] { return []; }
export func required(values: int[]): int {
  collect();
  return values.length == 0 ? 42 : values[0] + values[1];
}
export func nullable(values: int[]?): int {
  collect();
  return values == null || values.length == 0 ? 42 : values[0] + values[1];
}
)");
  ASSERT_TRUE(published) << published.error();
  auto& dispatch = isolate_.native_dispatch();
  const auto integers = dispatch.FindEntry("main.vela", "integers");
  const auto decimals = dispatch.FindEntry("main.vela", "decimals");
  const auto empty = dispatch.FindEntry("main.vela", "empty");
  const auto required = dispatch.FindEntry("main.vela", "required");
  const auto nullable = dispatch.FindEntry("main.vela", "nullable");
  ASSERT_TRUE(integers);
  ASSERT_TRUE(decimals);
  ASSERT_TRUE(empty);
  ASSERT_TRUE(required);
  ASSERT_TRUE(nullable);
  std::vector<runtime::HostRoot> arrays;
  for (const auto* factory : {&*integers, &*empty, &*decimals}) {
    const auto created = factory->Invoke({});
    ASSERT_TRUE(created);
    auto rooted = runtime::HostRoot::Create(isolate_, *created);
    ASSERT_TRUE(rooted);
    arrays.push_back(std::move(*rooted));
  }
  for (const auto* entry : {&*required, &*nullable}) {
    for (std::size_t index = 0; index < 2; ++index) {
      const std::array arguments{arrays[index].value()};
      const auto result = entry->Invoke(arguments);
      ASSERT_TRUE(result);
      EXPECT_EQ(result->AsInteger(), 42);
    }
  }
  const std::array null{runtime::Value::Null()};
  const auto nullable_result = nullable->Invoke(null);
  ASSERT_TRUE(nullable_result);
  EXPECT_EQ(nullable_result->AsInteger(), 42);

  runtime::Isolate other(1);
  const auto* layout = other.CreateArrayLayout({.element = {.kind = runtime::ValueKind::Integer}});
  auto* foreign = other.heap().AllocateArray(*layout, 2);
  ASSERT_NE(foreign, nullptr);
  const auto foreign_root = runtime::HostRoot::Create(other, runtime::Value::Object(foreign));
  ASSERT_TRUE(foreign_root);
  const std::array invalid{runtime::Value::Null(), runtime::Value::Integer(42),
                           runtime::Value::Object(nullptr), arrays.back().value(),
                           foreign_root->value()};
  for (const auto value : invalid) {
    const std::array arguments{value};
    const auto rejected = required->Invoke(arguments);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), ExecutionStatus::InvalidCast);
  }
}

TEST_P(JitCallableTest, CapturedArrayEntriesKeepBindingsRootsAndValidationAcrossPublication) {
  ASSERT_TRUE(
      isolate_.RegisterNative(runtime::NativeFunction::Bind<CollectCallbackInputs>("collect")));
  const auto first = R"(
external func collect(): void;
export func input(): int[] { return [40, 2]; }
export func empty(): int[] { return []; }
export func decimals(): double[] { return [40.0, 2.0]; }
export func required(values: int[]): int {
  collect();
  return values.length == 0 ? 42 : values[0] + values[1];
}
export func nullable(values: int[]?): int { collect(); return values == null ? 42 : values[0]; }
)";
  ASSERT_TRUE(Publish(first));
  auto& dispatch = isolate_.native_dispatch();
  const auto named = dispatch.FindEntry("main.vela", "required");
  const auto nullable_named = dispatch.FindEntry("main.vela", "nullable");
  ASSERT_TRUE(named);
  ASSERT_TRUE(nullable_named);
  const auto captured = named->Capture();
  const auto nullable = nullable_named->Capture();
  ASSERT_TRUE(captured);
  ASSERT_TRUE(nullable);
  const auto check_input = [&](std::string_view factory_name) {
    const auto factory = dispatch.FindEntry("main.vela", factory_name);
    ASSERT_TRUE(factory);
    const auto input = factory->Invoke({});
    ASSERT_TRUE(input);
    // 仅调用实参保存该数组，callee 的 collect 验证适配器在进入代码前已保根。
    const std::array arguments{*input};
    const auto result = captured->Invoke(arguments);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->AsInteger(), 42);
    const std::array extra{*input, *input};
    const auto rejected = captured->Invoke(extra);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), ExecutionStatus::InvalidEntry);
  };
  ASSERT_NO_FATAL_FAILURE(check_input("input"));
  ASSERT_NO_FATAL_FAILURE(check_input("empty"));
  const auto missing = captured->Invoke({});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), ExecutionStatus::InvalidEntry);
  const std::array null{runtime::Value::Null()};
  const auto optional_result = nullable->Invoke(null);
  ASSERT_TRUE(optional_result);
  EXPECT_EQ(optional_result->AsInteger(), 42);
  for (const auto value :
       {runtime::Value::Null(), runtime::Value::Integer(42), runtime::Value::Object(nullptr)}) {
    const std::array arguments{value};
    const auto rejected = captured->Invoke(arguments);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), ExecutionStatus::InvalidCast);
  }
  const auto decimals = dispatch.FindEntry("main.vela", "decimals");
  ASSERT_TRUE(decimals);
  const auto wrong_array = decimals->Invoke({});
  ASSERT_TRUE(wrong_array);
  const std::array wrong{*wrong_array};
  const auto rejected = captured->Invoke(wrong);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), ExecutionStatus::InvalidCast);
  runtime::Isolate foreign;
  const auto* foreign_layout =
      foreign.CreateArrayLayout({.element = {.kind = runtime::ValueKind::Integer}});
  auto* foreign_array = foreign.heap().AllocateArray(*foreign_layout, 0);
  ASSERT_NE(foreign_array, nullptr);
  const auto foreign_root =
      runtime::HostRoot::Create(foreign, runtime::Value::Object(foreign_array));
  ASSERT_TRUE(foreign_root);
  const std::array foreign_arguments{foreign_root->value()};
  const auto foreign_result = captured->Invoke(foreign_arguments);
  ASSERT_FALSE(foreign_result);
  EXPECT_EQ(foreign_result.error(), ExecutionStatus::InvalidCast);
  ASSERT_TRUE(Publish(R"(
external func collect(): void;
export func another(): int { return 7; }
export func nullable(values: int[]?): int { return 7; }
export func input(): int[] { return [40, 2]; }
export func empty(): int[] { return []; }
export func decimals(): double[] { return [40.0, 2.0]; }
export func required(values: int[]): int { collect(); return values.length + 41; }
)"));
  isolate_.heap().Collect();
  ASSERT_NO_FATAL_FAILURE(check_input("input"));
  const auto factory = dispatch.FindEntry("main.vela", "input");
  ASSERT_TRUE(factory);
  const auto current_input = factory->Invoke({});
  ASSERT_TRUE(current_input);
  const std::array arguments{*current_input};
  const auto current = named->Invoke(arguments);
  ASSERT_TRUE(current);
  EXPECT_EQ(current->AsInteger(), 43);
}

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
  const auto receiver_storage = runtime::HostRoot::Create(isolate_, *created);
  ASSERT_TRUE(receiver_storage);
  const auto& receiver = *receiver_storage;
  const std::array receiver_argument{receiver.value()};
  const std::array names{
      "topFactory", "lambdaFactory", "boundFactory", "interfaceFactory", "dynamicFactory",
  };
  std::vector<runtime::NativeVersionEntry> factories;
  std::vector<runtime::HostRoot> callbacks;
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
    auto root = runtime::HostRoot::Create(isolate_, *callback);
    ASSERT_TRUE(root);
    callbacks.push_back(std::move(*root));
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
    const auto callback_storage = runtime::HostRoot::Create(isolate_, *created_callback);
    ASSERT_TRUE(callback_storage);
    const auto& callback = *callback_storage;
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

TEST_P(JitCallableTest, LocalDirectCallsPreserveCapturesReassignmentAndVersionSelection) {
  const std::string source = R"(
func step(value: int): int { return value + 1; }
export func named(): int { const operation = step; return operation(40); }
export func captured(): int {
  const offset = 2;
  const operation = (value: int): int => value + offset;
  return operation(40);
}
export func reassigned(): int {
  let operation: (int) => int = step;
  operation = (value: int): int => value + 2;
  return operation(40);
}
)";
  const auto initial = Publish(source);
  ASSERT_TRUE(initial) << initial.error();
  auto& dispatch = isolate_.native_dispatch();
  const auto named = dispatch.FindEntry("main.vela", "named");
  ASSERT_TRUE(named);
  ASSERT_TRUE(test::ReturnsInteger(*named, 41));
  const auto old_named = named->Capture();
  ASSERT_TRUE(old_named);
  for (const auto* const name : {"captured", "reassigned"}) {
    const auto entry = dispatch.FindEntry("main.vela", name);
    ASSERT_TRUE(entry);
    ASSERT_TRUE(test::ReturnsInteger(*entry, 42));
  }
  auto changed = source;
  const auto previous = changed.find("return value + 1;");
  ASSERT_NE(previous, std::string::npos);
  changed.replace(previous, std::string_view("return value + 1;").size(), "return value + 2;");
  const auto updated = Publish(std::move(changed));
  ASSERT_TRUE(updated) << updated.error();
  isolate_.heap().Collect();
  ASSERT_TRUE(test::ReturnsInteger(*named, 42));
  const auto result = old_named->Invoke({});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 42);
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
  const auto previous_callback_storage = runtime::HostRoot::Create(isolate_, *created);
  ASSERT_TRUE(previous_callback_storage);
  const auto& previous_callback = *previous_callback_storage;
  const auto updated = Publish("func added(): string { return 'new'; } " + declarations +
                               "func increment(value: int): int { return value + 2; }");
  ASSERT_TRUE(updated) << updated.error();
  const auto created_after_update = old_factory->Invoke({});
  ASSERT_TRUE(created_after_update);
  const auto current_callback_storage = runtime::HostRoot::Create(isolate_, *created_after_update);
  ASSERT_TRUE(current_callback_storage);
  const auto& current_callback = *current_callback_storage;
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
  std::vector<runtime::HostRoot> callbacks;
  for (const auto* name : names) {
    const auto factory = dispatch.FindEntry("main.vela", name);
    ASSERT_TRUE(factory);
    const auto captured = factory->Capture();
    const auto callback = factory->Invoke({});
    ASSERT_TRUE(captured);
    ASSERT_TRUE(callback);
    factories.push_back(*captured);
    auto root = runtime::HostRoot::Create(isolate_, *callback);
    ASSERT_TRUE(root);
    callbacks.push_back(std::move(*root));
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
    const auto callback_storage = runtime::HostRoot::Create(isolate_, *created);
    ASSERT_TRUE(callback_storage);
    const auto& callback = *callback_storage;
    const std::array current_argument{callback.value()};
    ASSERT_TRUE(test::ReturnsInteger(*call, 42, current_argument));
  }
}

INSTANTIATE_TEST_SUITE_P(Tiers,
                         JitCallableTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::compiler::jit
