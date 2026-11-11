#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/hash/hash.h>
#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/project/project_config.h"
#include "compiler/test_support/program_test_support.h"
#include "compiler/test_support/reflection_test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

TEST(ReflectionContextTest, DistinguishesUnpublishedAndStoppedDispatch) {
  runtime::Isolate isolate;
  auto& dispatch = isolate.native_dispatch();
  const auto unpublished = Reflection::Bind(dispatch);
  ASSERT_FALSE(unpublished);
  EXPECT_EQ(unpublished.error().status, ExecutionStatus::InvalidEntry);
  dispatch.Close();
  const auto stopped = Reflection::Bind(dispatch);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().status, ExecutionStatus::ExecutionStopped);
  const auto type = Type<std::int64_t>::Bind(dispatch);
  ASSERT_FALSE(type);
  EXPECT_EQ(type.error().status, ExecutionStatus::ExecutionStopped);
}

class RuntimeReflectionTest : public ::testing::TestWithParam<NativeCodeTier> {
 protected:
  void Load(test::ReflectionInput source) {
    const frontend::SourceInput input{.path = "main.vela", .text = source.source};
    LoadModules(std::span(&input, 1), source.configuration);
  }
  void LoadModules(std::span<const frontend::SourceInput> inputs,
                   std::string_view configuration = "{}") {
    const auto config = ParseProjectConfiguration("vela.json", configuration);
    ASSERT_TRUE(config) << config.error().message;
    SourceOptions options;
    options.compilation.tier = GetParam();
    options.reflection = config->reflection;
    auto compiled = test::CompileModules(inputs, inputs.front().path, isolate_storage_, options);
    ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
    instance_storage_ = std::move(compiled.instance);
  }
  runtime::Isolate& isolate() { return isolate_storage_; }
  runtime::NativeInstance& instance() { return *instance_storage_; }
  void ReleaseInstance() { instance_storage_.reset(); }
  void ExpectAnswer() {
    const auto result = test::Execute(instance(), "answer");
    ASSERT_TRUE(result.Succeeded()) << result.message;
    EXPECT_EQ(result.value.AsInteger(), 42);
  }

 private:
  runtime::Isolate isolate_storage_{1};
  std::unique_ptr<runtime::NativeInstance> instance_storage_;
};

TEST_P(RuntimeReflectionTest, ConstructsAndUsesFieldsPropertiesMethodsAndStatics) {
  Load({
      .source = R"(
class Counter {
  value: int = 10;
  static total: int = 2;
  get amount(): int { return this.value; }
  set amount(value: int): void { this.value = value; }
  add(amount: int): int { this.value += amount; return this.value; }
  static twice(value: int): int { return value * 2; }
}
)",
      .configuration = R"({"reflection":[{"module":"main","type":"Counter","members":[
  {"kind":"constructor"},{"kind":"field","name":"value"},
  {"kind":"field","name":"total"},{"kind":"getter","name":"amount"},
  {"kind":"setter","name":"amount"},{"kind":"method","name":"add"},
  {"kind":"method","name":"twice"}
]}]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(instance().program().reflection, nullptr);
  auto reflection = Reflection::Bind(instance());
  ASSERT_TRUE(reflection);
  EXPECT_EQ(instance().program().reflection, nullptr);
  auto found = test::RequireReflectionValue(reflection->FindType("main.vela", "Counter"));
  ASSERT_TRUE(found) << found.error();
  const auto& type = *found;
  auto description = type.Describe();
  ASSERT_TRUE(description);
  EXPECT_TRUE(description->is_registered);
  EXPECT_EQ(description->name, "Counter");
  const auto constructor = test::RequireReflectionValue(type.GetConstructor());
  ASSERT_TRUE(constructor) << constructor.error();
  const auto parameters = constructor->GetParameters();
  ASSERT_TRUE(parameters);
  ASSERT_EQ(parameters->size(), 1U);
  EXPECT_TRUE(parameters->front().has_default);
  HandleScope scope(isolate());
  const auto object = constructor->Invoke(scope);
  ASSERT_TRUE(object) << object.error().message;
  const auto field = test::RequireReflectionValue(type.FindField("value"));
  ASSERT_TRUE(field) << field.error();
  auto value = field->GetValue(scope, *object);
  ASSERT_TRUE(value);
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 10);
  const auto twenty = CreateValue<std::int64_t>(scope, 20);
  ASSERT_TRUE(twenty);
  ASSERT_TRUE(field->SetValue(scope, *object, *twenty));
  const auto property = test::RequireReflectionValue(type.FindProperty("amount"));
  ASSERT_TRUE(property) << property.error();
  EXPECT_EQ(property->CanRead(), true);
  EXPECT_EQ(property->CanWrite(), true);
  value = property->GetValue(scope, *object);
  ASSERT_TRUE(value);
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 20);
  const auto method = test::RequireReflectionValue(type.FindMethod("add"));
  ASSERT_TRUE(method) << method.error();
  const std::array arguments{CallArgument(*twenty, "amount")};
  value = method->Invoke(scope, *object, arguments);
  ASSERT_TRUE(value) << value.error().message;
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 40);
  const auto null = CreateValue<Nullable<std::int64_t>>(scope, std::nullopt);
  ASSERT_TRUE(null);
  const auto total = test::RequireReflectionValue(type.FindField("total"));
  ASSERT_TRUE(total) << total.error();
  value = total->GetValue(scope, *null);
  ASSERT_TRUE(value);
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 2);
  ASSERT_TRUE(total->SetValue(scope, *null, *twenty));
  const auto twice = test::RequireReflectionValue(type.FindMethod("twice"));
  ASSERT_TRUE(twice) << twice.error();
  const std::array positional{CallArgument(*twenty)};
  value = twice->Invoke(scope, *null, positional);
  ASSERT_TRUE(value);
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 40);
  const auto wrong_receiver = total->GetValue(scope, *object);
  ASSERT_FALSE(wrong_receiver);
  EXPECT_EQ(wrong_receiver.error().status, ExecutionStatus::TypeError);
  const auto observed = reflection->TypeOf(scope, *object);
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed->id(), type.id());
  EXPECT_EQ(absl::HashOf(observed->id()), absl::HashOf(type.id()));
}

TEST_P(RuntimeReflectionTest, KeepsPrivateMetadataAndRejectsInvalidArgumentsBeforeUserCode) {
  Load({
      .source = R"(
class Box {
  value: int = 0;
  private hidden: int = 3;
  private set amount(value: int): void { this.value = value; }
  set only(value: int): void { this.value = value; }
  update(value: int, second: int): int { this.value += value; return second; }
}
)",
      .configuration = R"({"reflection":[{"module":"main","type":"Box","members":[
  {"kind":"constructor"},{"kind":"field","name":"value"},
  {"kind":"field","name":"hidden"},{"kind":"setter","name":"amount"},
  {"kind":"setter","name":"only"},{"kind":"method","name":"update"}
]}]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  const auto reflection = Reflection::Bind(instance());
  ASSERT_TRUE(reflection);
  const auto type = test::RequireReflectionValue(reflection->FindType("main.vela", "Box"));
  ASSERT_TRUE(type) << type.error();
  const auto constructor = test::RequireReflectionValue(type->GetConstructor());
  ASSERT_TRUE(constructor) << constructor.error();
  HandleScope scope(isolate());
  const auto object = constructor->Invoke(scope);
  ASSERT_TRUE(object);
  const auto hidden = test::RequireReflectionValue(
      type->FindField("hidden", {.visibility = MemberVisibilityFilter::Private}));
  ASSERT_TRUE(hidden) << hidden.error();
  const auto denied = hidden->GetValue(scope, *object);
  ASSERT_FALSE(denied);
  EXPECT_EQ(denied.error().status, ExecutionStatus::ReflectionAccessDenied);
  const auto method = test::RequireReflectionValue(type->FindMethod("update"));
  ASSERT_TRUE(method) << method.error();
  const auto integer = CreateValue<std::int64_t>(scope, 8);
  const auto invalid = CreateValue<double>(scope, 1.5);
  ASSERT_TRUE(integer && invalid);
  const std::array invalid_rest{CallArgument(*integer), CallArgument(*invalid)};
  const auto rejected = method->Invoke(scope, *object, invalid_rest);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, ExecutionStatus::InvalidCast);
  const std::array duplicate{CallArgument(*integer, "value"), CallArgument(*integer, "value")};
  const auto duplicated = method->Invoke(scope, *object, duplicate);
  ASSERT_FALSE(duplicated);
  EXPECT_EQ(duplicated.error().status, ExecutionStatus::TypeError);
  const auto field = test::RequireReflectionValue(type->FindField("value"));
  ASSERT_TRUE(field) << field.error();
  const auto value = field->GetValue(scope, *object);
  ASSERT_TRUE(value);
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 0);
}

TEST_P(RuntimeReflectionTest, StaticEnumFieldsPreserveDynamicBoxingAndNominalIdentity) {
  Load({
      .source = R"(
enum Color { red, blue }
[Reflect]
class Palette {
  static selected: Color = Color.blue;
  static optional: Color? = null;
}
export func answer(): int {
  const info = Reflect.typeInfo<Palette>();
  const selected = info.findField('selected');
  const optional = info.findField('optional');
  if (selected == null || optional == null) { return -1; }
  const value = selected.getValue(null);
  if (!(value is Color) || (value as Color) != Color.blue) { return -2; }
  if (optional.getValue(null) != null) { return -3; }
  optional.setValue(null, Color.red);
  if ((optional.getValue(null) as Color) != Color.red) { return -4; }
  return 42;
}
)",
      .configuration = "{}",
  });
  ASSERT_FALSE(HasFatalFailure());
  const auto reflection = Reflection::Bind(instance());
  ASSERT_TRUE(reflection);
  const auto palette = test::RequireReflectionValue(reflection->FindType("main.vela", "Palette"));
  ASSERT_TRUE(palette) << palette.error();
  const auto selected = test::RequireReflectionValue(palette->FindField("selected"));
  ASSERT_TRUE(selected) << selected.error();
  const auto expected_type = selected->GetType();
  ASSERT_TRUE(expected_type);
  HandleScope scope(isolate());
  const auto null = CreateValue<Nullable<std::int64_t>>(scope, std::nullopt);
  ASSERT_TRUE(null);
  const auto value = selected->GetValue(scope, *null);
  ASSERT_TRUE(value) << value.error().message;
  const auto actual_type = reflection->TypeOf(scope, *value);
  ASSERT_TRUE(actual_type);
  EXPECT_EQ(actual_type->id(), expected_type->id());
  ExpectAnswer();
}

TEST_P(RuntimeReflectionTest, PreservesEnumIdentityAndCrossIsolateBoundaries) {
  Load({
      .source = "enum Color { red, green, blue }",
      .configuration = R"({"reflection":[
    {"module":"main","type":"Color","members":[{"kind":"enumMember","name":"blue"}]}
  ]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  const auto reflection = Reflection::Bind(instance());
  ASSERT_TRUE(reflection);
  const auto type = test::RequireReflectionValue(reflection->FindType("main.vela", "Color"));
  ASSERT_TRUE(type) << type.error();
  const auto member = test::RequireReflectionValue(type->FindEnumMember("blue"));
  ASSERT_TRUE(member) << member.error();
  EXPECT_EQ(member->Ordinal(), 2);
  HandleScope scope(isolate());
  const auto value = member->GetValue(scope);
  ASSERT_TRUE(value);
  EXPECT_TRUE(internal::HandleAccess::Read(*value).IsObject());
  const auto actual = reflection->TypeOf(scope, *value);
  ASSERT_TRUE(actual);
  EXPECT_EQ(actual->id(), type->id());
  runtime::Isolate other;
  HandleScope other_scope(other);
  const auto other_value = CreateValue<std::int64_t>(other_scope, 2);
  ASSERT_TRUE(other_value);
  const auto mixed = reflection->TypeOf(scope, *other_value);
  ASSERT_FALSE(mixed);
  EXPECT_EQ(mixed.error().status, ExecutionStatus::InvalidIsolate);
}

TEST_P(RuntimeReflectionTest, GlobalStandardApiQueriesAndExecutesWithoutAnImport) {
  Load({
      .source = R"(
class Counter {
  value: int = 10;
  add(amount: int): int { this.value += amount; return this.value; }
}
export func answer(): int {
  const info = Reflect.typeInfo<Counter>();
  if (info.name != 'Counter' || !info.isRegistered || info.kind != TypeKind.Class) { return -1; }
  const ctor = info.getConstructor();
  if (ctor == null) { return -2; }
  if (!ctor.parameters[0].hasDefault || ctor.parameters[0].position != 0) { return -3; }
  const object = ctor.invoke(CallArguments());
  const field = info.findField('value');
  if (field == null) { return -4; }
  field.setValue(object, 20);
  const method = info.findMethod('add');
  if (method == null || method.parameters[0].name != 'amount') { return -5; }
  if (method.kind != MemberKind.Method || method.returnType.kind != TypeKind.Int) { return -6; }
  const repeated = info.findField('value');
  if (repeated == null || info.id != Reflect.typeOf(object).id || field.id != repeated.id) { return -7; }
  if (Reflect.typeInfo<void>().kind != TypeKind.Void || Reflect.typeOf(null).kind != TypeKind.Null) { return -8; }
  if (Reflect.getTypes('main.vela').length != 1 || Reflect.findType('main.vela', 'Counter') == null) { return -9; }
  const values = Set<TypeId>();
  values.add(info.id);
  if (!values.has(Reflect.typeOf(object).id)) { return -10; }
  return method.invoke(object, CallArguments(items: [CallArgument(value: 5, name: 'amount')])) as int;
}
)",
      .configuration = R"({"reflection":[{"module":"main","type":"Counter","members":[
  {"kind":"constructor"},{"kind":"field","name":"value"},{"kind":"method","name":"add"}
]}]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  const auto result = test::Execute(instance(), "answer");
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 25);
}

TEST_P(RuntimeReflectionTest, StructuralTypesAndForwardedGenericDescriptionsPreserveTheirGraphs) {
  Load({
      .source = R"(
class Box { value: int = 1; echo<T>(value: T): T { return value; } }
class Generic<T> { value: T; }
func describe<T>(): TypeInfo { return Reflect.typeInfo<T>(); }
export func answer(): int {
  const info = describe<Box>();
  const signature = Reflect.typeInfo<(int) => string>().signature;
  if (signature == null || signature.parameters[0].type.kind != TypeKind.Int) { return -1; }
  const generic = Reflect.typeInfo<Generic<(int) => string>>();
  if (generic.typeArguments[0].id != Reflect.typeInfo<(int) => string>().id) { return -5; }
  const values: Box[] = [];
  if (Reflect.typeOf(values).typeArguments[0].id != info.id) { return -2; }
  const method = info.findMethod('echo', typeArguments: [Reflect.typeInfo<string>()]);
  if (method == null) { return -3; }
  const callable = info.findMethod('echo', typeArguments: [Reflect.typeInfo<(int) => string>()]);
  if (callable == null || callable.typeArguments[0].id != generic.typeArguments[0].id) { return -6; }
  const result = method.invoke(Box(), CallArguments(items: [CallArgument(value: 'ok')])) as string;
  return result == 'ok' ? 42 : -4;
}
)",
      .configuration = R"({"reflection":[{"module":"main","type":"Box","members":[
  {"kind":"method","name":"echo","typeArguments":["string"]},
  {"kind":"method","name":"echo","typeArguments":["(int) => string"]}
]}]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  ExpectAnswer();
}

TEST(RuntimeReflectionBuiltinTest, DescribesAndChecksStructuralHostTypesWithoutAProgram) {
  runtime::Isolate isolate(1);
  const auto integer = Type<std::int64_t>::Bind(isolate);
  const auto nullable = Type<Nullable<std::int64_t>>::Bind(isolate);
  const auto array = Type<Array<Future<std::int64_t>>>::Bind(isolate);
  const auto function = Type<Function<String(std::int64_t)>>::Bind(isolate);
  ASSERT_TRUE(integer && nullable && array && function);
  const auto info = integer->Describe();
  const auto nullable_info = nullable->Describe();
  const auto array_info = array->Describe();
  const auto function_info = function->Describe();
  ASSERT_TRUE(info && nullable_info && array_info && function_info);
  const auto description = info->Describe();
  ASSERT_TRUE(description) << description.error().message;
  EXPECT_EQ(description->kind, TypeKind::Int);
  EXPECT_FALSE(description->is_registered);
  EXPECT_EQ(info->GetFields()->size(), 0U);
  EXPECT_EQ(info->FindMethod("missing"), std::nullopt);
  const auto enumeration = info->GetEnumMembers();
  ASSERT_FALSE(enumeration);
  EXPECT_EQ(enumeration.error().status, ExecutionStatus::TypeError);
  const auto elements = array_info->Describe();
  ASSERT_TRUE(elements);
  ASSERT_EQ(elements->type_arguments.size(), 1U);
  const auto payload = elements->type_arguments.front().Describe();
  ASSERT_TRUE(payload);
  EXPECT_EQ(payload->kind, TypeKind::Future);
  ASSERT_EQ(payload->type_arguments.size(), 1U);
  EXPECT_EQ(payload->type_arguments.front().id(), info->id());
  const auto signature = function_info->Describe();
  ASSERT_TRUE(signature);
  const auto& function_signature = signature->signature;
  if (!function_signature) {
    FAIL() << "function type description omitted its signature";
  }
  EXPECT_EQ(function_signature->parameters.front().type.id(), info->id());
  HandleScope scope(isolate);
  const auto value = CreateValue<std::int64_t>(scope, 42);
  const auto null = CreateValue<Nullable<std::int64_t>>(scope, std::nullopt);
  ASSERT_TRUE(value && null);
  EXPECT_EQ(info->IsInstance(scope, *value), true);
  EXPECT_EQ(info->IsInstance(scope, *null), false);
  EXPECT_EQ(nullable_info->IsInstance(scope, *null), true);
  EXPECT_EQ(nullable_info->IsAssignableFrom(*info), true);
  EXPECT_EQ(info->IsAssignableFrom(*nullable_info), false);
}

TEST_P(RuntimeReflectionTest, InheritedQueriesRespectOverridesOwnersAndInterfaceDispatch) {
  Load({
      .source = R"(
interface Reader { read(): int; }
class Base {
  base: int = 10;
  private hidden: int = 1;
  static total: int = 3;
  read(): int { return 1; }
}
class Derived extends Base implements Reader {
  last: int = 20;
  override read(): int { return 2; }
}
export func answer(): int {
  const info = Reflect.typeInfo<Derived>();
  const base = Reflect.typeInfo<Base>();
  const object = Derived();
  if (!base.isAssignableFrom(info) || info.isAssignableFrom(base)) { return -1; }
  if (!base.isInstance(object) || !Reflect.typeInfo<Reader>().isInstance(object)) { return -2; }
  const fields = info.getFields(MemberQuery(storage: MemberStorageFilter.Instance));
  if (fields.length != 2 || fields[0].name != 'base' || fields[1].name != 'last') { return -3; }
  if (fields[0].declaringType.id != base.id) { return -4; }
  if (info.getFields(MemberQuery(scope: MemberScope.Declared)).length != 1) { return -5; }
  if (info.getFields(MemberQuery(visibility: MemberVisibilityFilter.All)).length != 4) { return -6; }
  if (info.findMethod('read') != null) { return -7; }
  const declared = base.findMethod('read');
  const interfaceMethod = Reflect.typeInfo<Reader>().findMethod('read');
  if (declared == null || interfaceMethod == null) { return -8; }
  if (declared.invoke(object, CallArguments()) as int != 1) { return -9; }
  if (interfaceMethod.invoke(object, CallArguments()) as int != 2) { return -10; }
  return 42;
}
)",
      .configuration = R"({"reflection":[
 {"module":"main","type":"Base","members":[
  {"kind":"field","name":"base"},{"kind":"field","name":"hidden"},
  {"kind":"field","name":"total"},{"kind":"method","name":"read"}]},
 {"module":"main","type":"Derived","members":[{"kind":"field","name":"last"}]},
 {"module":"main","type":"Reader","members":[{"kind":"method","name":"read"}]}
]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  ExpectAnswer();
}

TEST_P(RuntimeReflectionTest, ConstructorsValidateBeforeDefaultsAndPreserveNullAndUserExceptions) {
  Load({
      .source = R"(
let defaults = 0;
let bodies = 0;
const expected = Error(message: 'original', code: 'Original');
func next(): int { defaults++; return 10; }
class Generated {
  required: string?;
  value: int = next();
  fail(): void { throw expected; }
  clear(): void { this.value = 0; }
}
class Explicit {
  value: int;
  constructor(value: int) { bodies++; this.value = value; }
}
export func answer(): int {
  const info = Reflect.typeInfo<Generated>();
  const ctor = info.getConstructor() as ConstructorInfo;
  const explicitCtor = Reflect.typeInfo<Explicit>().getConstructor() as ConstructorInfo;
  try { ctor.invoke(CallArguments()); return -2; }
  catch (error: Error) { if (error.code != 'TypeError') { return -3; } }
  try { ctor.invoke(CallArguments(items: [CallArgument(value: 2, name: 'required')])); return -4; }
  catch (error: Error) { if (error.code != 'InvalidCast') { return -5; } }
  if (defaults != 0 || bodies != 0) { return -6; }
  const object = ctor.invoke(CallArguments(items: [CallArgument(value: null, name: 'required')]));
  if (defaults != 1) { return -7; }
  const value = info.findField('value') as FieldInfo;
  const required = info.findField('required') as FieldInfo;
  const fail = info.findMethod('fail') as MethodInfo;
  const clear = info.findMethod('clear') as MethodInfo;
  if (required.getValue(object) != null || value.getValue(object) as int != 10) { return -9; }
  try { fail.invoke(object, CallArguments()); return -10; }
  catch (error: Error) { if (error != expected) { return -11; } }
  if (clear.invoke(object, CallArguments()) != null || value.getValue(object) as int != 0) { return -12; }
  try { explicitCtor.invoke(CallArguments(items: [CallArgument(value: 42)])); return -13; }
  catch (error: Error) { if (error.code != 'TypeError') { return -14; } }
  const created = explicitCtor.invoke(CallArguments(items: [CallArgument(value: 42, name: 'value')]));
  const actual = created as Explicit;
  if (bodies != 1 || actual.value != 42 || defaults != 1) { return -15; }
  return 42;
}
)",
      .configuration = R"({"reflection":[
 {"module":"main","type":"Generated","members":[{"kind":"constructor"},
  {"kind":"field","name":"required"},{"kind":"field","name":"value"},
  {"kind":"method","name":"fail"},{"kind":"method","name":"clear"}]},
 {"module":"main","type":"Explicit","members":[{"kind":"constructor"}]}
]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  ExpectAnswer();
}

TEST_P(RuntimeReflectionTest, AsyncCallsReturnTheOriginalFutureWithItsPayloadContract) {
  Load({
      .source = R"(
const source = Completer<int>();
const original = source.future;
class Worker {
  fetch(): Future<int> { return original; }
  async work(): Future<int> { return await original; }
}
async func main(): Future<int> {
  const info = Reflect.typeInfo<Worker>();
  const fetch = info.findMethod('fetch');
  const work = info.findMethod('work');
  if (fetch == null || work == null || fetch.isAsync || !work.isAsync) { return -1; }
  if (work.returnType.kind != TypeKind.Future || work.returnType.typeArguments[0].kind != TypeKind.Int) { return -2; }
  const object = Worker();
  const returned = fetch.invoke(object, CallArguments()) as Future<int>;
  if (returned != original) { return -3; }
  const pending = work.invoke(object, CallArguments()) as Future<int>;
  source.complete(42);
  return await pending;
}
)",
      .configuration = R"({"reflection":[{"module":"main","type":"Worker","members":[
 {"kind":"method","name":"fetch"},{"kind":"method","name":"work"}
]}]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  auto result = test::StartFuture(instance());
  isolate().heap().Collect();
  const auto completed = test::AwaitInteger(result, isolate());
  ASSERT_TRUE(completed) << completed.error();
  EXPECT_EQ(*completed, 42);
}

TEST_P(RuntimeReflectionTest, RegisteredStandardStaticsRespectReadonlyFieldsAndBuiltinSignatures) {
  Load({
      .source = R"(
export func answer(): int {
  const info = Reflect.typeInfo<Math>();
  const constant = info.findField('PI');
  const sqrt = info.findMethod('sqrt') as MethodInfo;
  if (constant == null || !info.isRegistered) { return -1; }
  if (constant.getValue(null) as double != Math.PI) { return -2; }
  try { constant.setValue(null, 0.0); return -3; }
  catch (error: Error) { if (error.code != 'ReflectionAccessDenied') { return -4; } }
  if (sqrt.invoke(null, CallArguments(items: [CallArgument(value: 9.0)])) as double != 3.0) { return -5; }
  const integer = Reflect.typeInfo<int>();
  const parse: MethodInfo = integer.findMethod('parse') as MethodInfo;
  if (!integer.isRegistered) { return -6; }
  if (!parse.parameters[1].isOptional || parse.parameters[1].hasDefault) { return -7; }
  if (parse.invoke(null, CallArguments(items: [CallArgument(value: '2a'), CallArgument(value: 16, name: 'radix')])) as int != 42) { return -8; }
  if (!parse.parameters[1].type.isNullable) { return -9; }
  if (parse.invoke(null, CallArguments(items: [CallArgument(value: '42'), CallArgument(value: null)])) as int != 42) { return -10; }
  try { parse.invoke(null, CallArguments(items: [CallArgument(value: '42'), CallArgument(value: 1.5)])); return -11; }
  catch (error: Error) { if (error.code != 'InvalidCast') { return -12; } }
  return parse.invoke(null, CallArguments(items: [CallArgument(value: '42')])) as int;
}
)",
      .configuration = R"({"reflection":[
 {"module":"builtin:Math","type":"Math","members":[
  {"kind":"field","name":"PI"},{"kind":"method","name":"sqrt"}]},
 {"module":"builtin:int","type":"int","members":[{"kind":"method","name":"parse"}]}
]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  ExpectAnswer();
}

TEST_P(RuntimeReflectionTest, UnreachableProgramAndManagedDescriptorCyclesAreCollected) {
  Load({
      .source = R"(
class Button extends Reflect { label: string = 'saved'; }
class Box { value: int = 42; [Button] read(): int { return this.value; } }
let saved: TypeInfo? = null;
let field: FieldInfo? = null;
let matches: MethodAttributeMatch<Button>[]? = null;
export func remember(): void {
  saved = Reflect.typeInfo<Box>();
  field = Reflect.typeInfo<Box>().findField('value');
  matches = Reflect.findMethods<Button>();
}
)",
      .configuration = R"({"reflection":[{"module":"main","type":"Box","members":[
 {"kind":"field","name":"value"}
]}]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  const auto result = test::Execute(instance(), "remember");
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_GT(isolate().heap().object_count(), 0U);
  ReleaseInstance();
  isolate().heap().Collect();
  EXPECT_EQ(isolate().heap().object_count(), 0U);
}

TEST_P(RuntimeReflectionTest, MetadataQueriesArePureAndAccessorFailuresRemainDistinct) {
  Load({
      .source = R"(
let initialized = 0;
let setters = 0;
func next(): int { initialized++; return 42; }
class Box {
  static value: int = next();
  get readOnly(): int { return 1; }
  get omittedSetter(): int { return 2; }
  set omittedSetter(value: int): void { setters++; }
  set onlySetter(value: int): void { setters++; }
  get failing(): int { throw Error(message: 'getter failed', code: 'GetterFailure'); }
}
export func answer(): int {
  const info = Reflect.typeInfo<Box>();
  const fields = info.getFields();
  const properties = info.getProperties();
  if (initialized != 0 || setters != 0) { return -1; }
  const field = info.findField('value') as FieldInfo;
  if (field.getValue(null) as int != 42 || field.getValue(null) as int != 42 || initialized != 1) { return -2; }
  const omitted = info.findProperty('omittedSetter') as PropertyInfo;
  if (omitted.canWrite || omitted.setter != null) { return -3; }
  try { omitted.setValue(Box(), 5); return -4; }
  catch (error: Error) { if (error.code != 'ReflectionUnavailable') { return -5; } }
  const readOnly = info.findProperty('readOnly') as PropertyInfo;
  try { readOnly.setValue(Box(), 5); return -6; }
  catch (error: Error) { if (error.code != 'TypeError') { return -7; } }
  const writeOnly = info.findProperty('onlySetter') as PropertyInfo;
  try { writeOnly.getValue(Box()); return -8; }
  catch (error: Error) { if (error.code != 'TypeError') { return -9; } }
  try { writeOnly.setValue(Box(), 1.5); return -10; }
  catch (error: Error) { if (error.code != 'InvalidCast') { return -11; } }
  const failing = info.findProperty('failing') as PropertyInfo;
  try { failing.getValue(Box()); return -12; }
  catch (error: Error) { if (error.code != 'GetterFailure') { return -13; } }
  return initialized == 1 && setters == 0 ? 42 : -14;
}
)",
      .configuration = R"({"reflection":[{"module":"main","type":"Box","members":[
 {"kind":"field","name":"value"},{"kind":"getter","name":"readOnly"},
 {"kind":"getter","name":"omittedSetter"},{"kind":"setter","name":"onlySetter"},
 {"kind":"getter","name":"failing"}
]}]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  ExpectAnswer();
}

TEST_P(RuntimeReflectionTest, SourceAttributesRegisterMembersAndCopyMethodData) {
  Load({
      .source = R"(
[AttributeUsage(allowMultiple: true)]
class Button extends Reflect { label: string = ''; }
class PrimaryButton extends Button { priority: int = 7; }
class Note extends Attribute {}
[Reflect]
class Whole {
  value: int = 1;
  private secret: int = 2;
  get amount(): int { return this.value; }
  private set amount(value: int): void { this.value = value; }
  [Note]
  plain(): int { return 3; }
}
class Counter {
  value: int = 0;
  [Button(label: 'first')]
  [Button(label: 'second')]
  reset(): int { this.value = 42; return this.value; }
  [PrimaryButton]
  static run(): int { return 42; }
  [Button(label: 'hidden')]
  private hidden(): int { return -1; }
  [Button(label: 'getter')]
  get amount(): int { return this.value; }
  [Note]
  ignored(): void {}
}
class Child extends Counter {}
export func answer(): int {
  const whole = Reflect.typeInfo<Whole>();
  if (!whole.isRegistered || whole.getFields().length != 1 || whole.getConstructor() == null) { return -1; }
  const amount = whole.findProperty('amount') as PropertyInfo;
  if (!amount.canRead || amount.canWrite || amount.setter != null) { return -2; }
  const counter = Reflect.findType('main.vela', 'Counter') as TypeInfo;
  if (counter.findField('value') != null || counter.getConstructor() != null) { return -3; }
  const matches = Reflect.findMethods<Button>();
  if (matches.length != 3 || Reflect.findMethods<Reflect>().length != 3) { return -4; }
  if (Reflect.findMethods<Note>().length != 1 || Reflect.findMethods<AttributeUsage>().length != 0 || Reflect.typeInfo<Button>().isRegistered) { return -11; }
  if (counter.findMethod('ignored') != null) { return -12; }
  if (matches[0].method.name != 'reset' || matches[0].attribute.label != 'first' || matches[1].attribute.label != 'second') { return -5; }
  if (matches[2].method.name != 'run' || !matches[2].method.isStatic || !(matches[2].attribute is PrimaryButton)) { return -6; }
  if ((matches[2].attribute as PrimaryButton).priority != 7) { return -7; }
  if (Reflect.findMethods<Button>(module: 'missing.vela').length != 0 || Reflect.findMethods<Button>(module: 'main.vela').length != 3) { return -8; }
  matches[0].attribute.label = 'changed';
  const again = Reflect.findMethods<Button>(module: null);
  if (again[0].attribute.label != 'first' || again[1].attribute.label != 'second') { return -9; }
  const object = Counter();
  if (matches[0].method.invoke(object, CallArguments()) as int != 42 || object.value != 42) { return -10; }
  return matches[2].method.invoke(null, CallArguments()) as int;
}
)",
      .configuration = "{}",
  });
  ASSERT_FALSE(HasFatalFailure());
  ExpectAnswer();
}

TEST_P(RuntimeReflectionTest, AttributeDataPreservesScalarTypesWithoutInitializationEffects) {
  Load({
      .source = R"(
enum Color { Red, Blue }
let effects = 0;
func initialize(): int { effects++; return 1; }
class Data extends Reflect {
  static effect: int = initialize();
  count: int = -42;
  ratio: double = -1.5;
  enabled: bool = true;
  title: string = 'data';
  color: Color = Color.Blue;
  boxed: dynamic = Color.Red;
  optional: int? = null;
}
class Target {
  [Data]
  constructor() { effects++; }
  [Data]
  static run(): int { effects++; return 42; }
}
export func answer(): int {
  const matches = Reflect.findMethods<Data>();
  if (matches.length != 1 || effects != 0) { return -1; }
  const data = matches[0].attribute;
  if (data.count != -42 || data.ratio != -1.5 || !data.enabled || data.title != 'data') { return -2; }
  if (data.color != Color.Blue || !(data.boxed is Color) || data.boxed as Color != Color.Red || data.optional != null) { return -3; }
  data.count = 7;
  if (Reflect.findMethods<Data>()[0].attribute.count != -42 || effects != 0) { return -4; }
  return matches[0].method.invoke(null, CallArguments()) as int;
}
)",
      .configuration = "{}",
  });
  ASSERT_FALSE(HasFatalFailure());
  ExpectAnswer();
}

TEST_P(RuntimeReflectionTest, AttributeDiscoveryUsesConcreteSpecializationsAndInterfaceSelectors) {
  Load({
      .source = R"(
class Tag<T> extends Reflect { label: string = 'generic'; }
[Reflect]
class Box<T> {
  value: T;
  [Tag<T>]
  echo<U>(value: U): U { return value; }
}
interface Action {
  [Tag<string>(label: 'interface')]
  run(): int;
}
class Implementation implements Action { run(): int { return 42; } }
export func answer(): int {
  const box = Box<int>(value: 40);
  if (box.echo<int>(2) != 2) { return -1; }
  const generic = Reflect.findMethods<Tag<int>>();
  if (generic.length != 1 || generic[0].method.name != 'echo' || generic[0].method.typeArguments.length != 1) { return -2; }
  if (generic[0].method.invoke(box, CallArguments(items: [CallArgument(value: 42)])) as int != 42) { return -3; }
  const actions = Reflect.findMethods<Tag<string>>();
  if (actions.length != 1 || actions[0].method.declaringType.name != 'Action') { return -4; }
  return actions[0].method.invoke(Implementation(), CallArguments()) as int;
}
)",
      .configuration = "{}",
  });
  ASSERT_FALSE(HasFatalFailure());
  ExpectAnswer();
}

TEST_P(RuntimeReflectionTest, AttributeDiscoveryOrdersByDeclaringModuleAndDeduplicatesInheritance) {
  const std::array sources{
      frontend::SourceInput{
          .path = "main.vela",
          .text = R"(
import 'zeta' as z;
import 'alpha' as a;
import 'attributes' as tags;
class Child extends a.Target { override run(): int { return 0; } }
class Other extends a.Target {}
export func answer(): int {
  const matches = Reflect.findMethods<Reflect>();
  if (matches.length != 2 || matches[0].method.declaringType.module != 'alpha.vela' || matches[1].method.declaringType.module != 'zeta.vela') { return -1; }
  if (Reflect.findMethods<Reflect>(module: 'alpha.vela').length != 1) { return -2; }
  const tagged = Reflect.findMethods<tags.Tag>();
  if (tagged.length != 2 || tagged[0].attribute.label != 'alpha' || tagged[1].attribute.label != 'zeta' || tagged[0].attribute.kind != tags.Kind.Active) { return -3; }
  return matches[0].method.invoke(Other(), CallArguments()) as int;
}
)",
      },
      frontend::SourceInput{
          .path = "zeta.vela",
          .text = "import 'attributes' as tags; export class Target { "
                  "[tags.Tag(label: 'zeta')] static run(): int { return 1; } }",
      },
      frontend::SourceInput{
          .path = "alpha.vela",
          .text = "import 'attributes' as tags; type Mark = tags.Tag; export class Target { "
                  "[Mark(label: 'alpha')] run(): int { return 42; } }",
      },
      frontend::SourceInput{
          .path = "attributes.vela",
          .text = "export enum Kind { Active } export class Tag extends Reflect "
                  "{ label: string = ''; kind: Kind = Kind.Active; }",
      },
  };
  LoadModules(sources);
  ASSERT_FALSE(HasFatalFailure());
  ExpectAnswer();
}

INSTANTIATE_TEST_SUITE_P(ReflectionTiers,
                         RuntimeReflectionTest,
                         ::testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::compiler::jit
