#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/native_binding.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/project/project_config.h"
#include "compiler/test_support/reflection_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_class.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_host_type.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"
#include "vela/error.h"

namespace vela::runtime::binding_test {

struct Counter final {
  explicit Counter(std::int64_t initial) noexcept : value(initial) {}
  std::int64_t Add(std::int64_t delta) noexcept { return value += delta; }
  std::int64_t GcAdd(NativeContext& context, std::int64_t delta) noexcept {
    context.isolate().heap().Collect();
    return Add(delta);
  }
  [[nodiscard]] std::string Text(std::string_view text) const noexcept {
    return std::to_string(value) + ":" + std::string(text);
  }
  std::int64_t value;
};

}  // namespace vela::runtime::binding_test

namespace vela::runtime {

template <>
struct NativeClassTraits<binding_test::Counter> final {
  static constexpr std::string_view kKey = "test.Counter";
};

}  // namespace vela::runtime

namespace vela::compiler::jit {
namespace {

using runtime::binding_test::Counter;

std::expected<runtime::NativeConstructed<Counter>, vela::Error> ConstructCounter(
    runtime::NativeContext& context,
    std::int64_t initial) noexcept {
  return context.Construct<Counter>(initial);
}

std::int64_t Add(Counter& receiver, std::int64_t delta) noexcept {
  return receiver.Add(delta);
}

std::int64_t GcAdd(runtime::NativeContext& context,
                   Counter& receiver,
                   std::int64_t delta) noexcept {
  return receiver.GcAdd(context, delta);
}

std::int64_t Value(const Counter& receiver) noexcept {
  return receiver.value;
}

void SetValue(Counter& receiver, std::int64_t amount) noexcept {
  receiver.value = amount;
}

std::unique_ptr<Counter> Create(std::int64_t initial) noexcept {
  return std::unique_ptr<Counter>(new (std::nothrow) Counter(initial));
}

std::int64_t Read(const Counter* counter) noexcept {
  return counter != nullptr ? counter->value : 0;
}

std::string Text(const Counter& counter, std::string_view text) noexcept {
  return counter.Text(text);
}

template <auto Function>
runtime::NativeClassMember Member(NativeMemberKind kind,
                                  std::string&& name,
                                  std::vector<std::string>&& parameters) {
  auto key = NativeMemberKey("test.Counter", kind, name);
  return {
      .kind = kind,
      .name = std::move(name),
      .parameter_names = std::move(parameters),
      .function = runtime::NativeFunction::Bind<Function>(std::move(key)),
  };
}

std::expected<void, runtime::NativeRegistrationError> RegisterCounter(runtime::Isolate& isolate) {
  std::vector<runtime::NativeClassMember> members;
  members.push_back(
      Member<&ConstructCounter>(NativeMemberKind::Constructor, "constructor", {"initial"}));
  members.push_back(Member<&Add>(NativeMemberKind::Method, "add", {"delta"}));
  members.push_back(Member<&GcAdd>(NativeMemberKind::Method, "gcAdd", {"delta"}));
  members.push_back(Member<&Value>(NativeMemberKind::Getter, "value", {}));
  members.push_back(Member<&SetValue>(NativeMemberKind::Setter, "value", {"amount"}));
  members.push_back(Member<&Create>(NativeMemberKind::Static, "Create", {"initial"}));
  members.push_back(Member<&Read>(NativeMemberKind::Static, "Read", {"counter"}));
  members.push_back(Member<&Text>(NativeMemberKind::Method, "text", {"text"}));
  std::vector<runtime::NativeClass> module;
  module.push_back(runtime::NativeClass::Define<Counter>(std::move(members)));
  return isolate.RegisterNativeModule(std::move(module));
}

constexpr std::string_view kSource = R"(
external class Counter = 'test.Counter' {
  constructor(initial: int);
  add(delta: int): int;
  gcAdd(delta: int): int;
  get value(): int;
  set value(amount: int): void;
  static Create(initial: int): Counter?;
  static Read(counter: Counter?): int;
  text(text: string): string;
}

func callback(): (int) => int {
  const counter = Counter(initial: 40);
  return counter.gcAdd;
}

export func boundOnly(): int {
  const call = callback();
  return call(2);
}

export func run(): int {
  const counter = Counter(initial: 1);
  if (counter.add(2) != 3) { return -1; }
  const bound = counter.gcAdd;
  if (bound(3) != 6) { return -2; }
  const dynamicCounter: dynamic = counter;
  if (dynamicCounter.gcAdd(4) != 10) { return -3; }
  counter.value = 42;
  if (Counter.Read(null) != 0 || Counter.Read(counter) != 42) { return -4; }
  const created = Counter.Create(initial: 7);
  if (created == null || created.value != 7) { return -5; }
  if (counter.text('hello') != '42:hello') { return -6; }
  const values: Counter[] = [counter, created];
  if (!(dynamicCounter is Counter)) { return -7; }
  return values[0].value;
}

let active = Counter(initial: 1);
export func loopedBorrow(): int {
  const first = Counter(initial: 1);
  const second = Counter(initial: 2);
  let sum = 0;
  for (let index = 0; index < 21; index++) {
    active = index % 2 == 0 ? first : second;
    sum += active.gcAdd(0);
    let argument: Counter? = null;
    if (index % 2 == 0) { argument = active; }
    sum += Counter.Read(argument);
  }
  return sum;
}
)";

class NativeClassJitTest : public ::testing::TestWithParam<NativeCodeTier> {};

TEST_P(NativeClassJitTest, ExecutesOwnedConstructionMembersPropertiesDynamicCallsAndBoundMethods) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler.has_value());
  const auto compiled =
      CompileSource(**compiler, kSource,
                    {.diagnostics = {}, .compilation = {.tier = GetParam()}, .reflection = {}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate isolate(1);
  ASSERT_TRUE(RegisterCounter(isolate).has_value());
  static_cast<void>(isolate.FreezeBindings());
  auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_TRUE(instance.has_value()) << instance.error().message;
  runtime::NativeExecution execution(**instance);
  for (const auto* const name : {"run", "boundOnly", "loopedBorrow"}) {
    const auto function = (*instance)->FindFunction(name);
    if (!function) {
      FAIL() << "expected exported function " << name;
    }
    const auto returned = runtime::InvokeNativeFunction(execution, *function);
    ASSERT_TRUE(returned.has_value()) << static_cast<unsigned>(returned.error());
    EXPECT_EQ(returned->AsInteger(), 42);
  }
  isolate.heap().Collect();
}

TEST_P(NativeClassJitTest, RejectsMissingTypesAndParameterNameDriftBeforePublication) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler.has_value());
  const auto compiled =
      CompileSource(**compiler, kSource,
                    {.diagnostics = {}, .compilation = {.tier = GetParam()}, .reflection = {}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate missing;
  const auto absent = runtime::NativeInstance::Instantiate(missing, compiled.code.image);
  ASSERT_FALSE(absent);
  EXPECT_EQ(absent.error().status, runtime::NativeInstantiationStatus::MissingImport);

  std::string source(kSource);
  const auto parameter = source.find("add(delta: int)");
  ASSERT_NE(parameter, std::string::npos);
  source.replace(parameter, std::string_view("add(delta: int)").size(), "add(amount: int)");
  const auto changed =
      CompileSource(**compiler, source,
                    {.diagnostics = {}, .compilation = {.tier = GetParam()}, .reflection = {}});
  ASSERT_TRUE(changed.Succeeded()) << frontend::DiagnosticsText(changed.diagnostics);
  runtime::Isolate isolate;
  ASSERT_TRUE(RegisterCounter(isolate).has_value());
  const auto mismatch = runtime::NativeInstance::Instantiate(isolate, changed.code.image);
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error().status, runtime::NativeInstantiationStatus::ImportSignatureMismatch);
}

TEST_P(NativeClassJitTest, RejectsDriftInUnusedMembersAsPartOfTheWholeClassContract) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler.has_value());
  auto source = std::string(kSource);
  const auto method = source.find("  text(text: string): string;");
  ASSERT_NE(method, std::string::npos);
  source.insert(method, "  unused(value: bool): bool;\n");
  const auto compiled =
      CompileSource(**compiler, source,
                    {.diagnostics = {}, .compilation = {.tier = GetParam()}, .reflection = {}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate isolate;
  ASSERT_TRUE(RegisterCounter(isolate));
  const auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_FALSE(instance);
  EXPECT_EQ(instance.error().status, runtime::NativeInstantiationStatus::ImportSignatureMismatch);
  EXPECT_TRUE(instance.error().message.contains("class contract"));
}

TEST_P(NativeClassJitTest, ReflectionUsesBoundNativeConstructionPropertiesAndGcCheckedMethods) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  const auto configuration = ParseProjectConfiguration("vela.json", R"({"reflection":[
    {"module":"main","type":"Counter","members":[{"kind":"constructor"},
      {"kind":"method","name":"gcAdd"},{"kind":"getter","name":"value"},
      {"kind":"setter","name":"value"}]}
  ]})");
  ASSERT_TRUE(configuration);
  const frontend::SourceInput source{.path = "main.vela", .text = kSource};
  SourceOptions options;
  options.compilation.tier = GetParam();
  options.reflection = configuration->reflection;
  const auto compiled = CompileModules(**compiler, std::span(&source, 1), source.path, options);
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate isolate(1);
  ASSERT_TRUE(RegisterCounter(isolate));
  auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_TRUE(instance) << instance.error().message;
  const auto reflection = Reflection::Bind(**instance);
  ASSERT_TRUE(reflection);
  const auto type = test::RequireReflectionValue(reflection->FindType("main.vela", "Counter"));
  ASSERT_TRUE(type) << type.error();
  const auto bound = Type<Host<Counter>>::Bind(**instance);
  ASSERT_TRUE(bound);
  const auto described = bound->Describe();
  ASSERT_TRUE(described);
  EXPECT_EQ(described->id(), type->id());
  const auto fields = type->GetFields();
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_TRUE(fields->empty());
  const auto constructor = test::RequireReflectionValue(type->GetConstructor());
  ASSERT_TRUE(constructor) << constructor.error();
  const auto method = test::RequireReflectionValue(type->FindMethod("gcAdd"));
  ASSERT_TRUE(method) << method.error();
  const auto property = test::RequireReflectionValue(type->FindProperty("value"));
  ASSERT_TRUE(property) << property.error();
  HandleScope scope(isolate);
  const auto initial = CreateValue<std::int64_t>(scope, 40);
  const auto delta = CreateValue<std::int64_t>(scope, 2);
  ASSERT_TRUE(initial && delta);
  const std::array parameters{CallArgument(*initial, "initial")};
  const auto object = constructor->Invoke(scope, parameters);
  ASSERT_TRUE(object) << object.error().message;
  const std::array arguments{CallArgument(*delta, "delta")};
  const auto value = method->Invoke(scope, *object, arguments);
  ASSERT_TRUE(value) << value.error().message;
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 42);
  ASSERT_TRUE(property->SetValue(scope, *object, *initial));
  const auto read = property->GetValue(scope, *object);
  ASSERT_TRUE(read);
  EXPECT_EQ(internal::HandleAccess::Read(*read).AsInteger(), 40);
}

INSTANTIATE_TEST_SUITE_P(Tiers,
                         NativeClassJitTest,
                         ::testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::compiler::jit
