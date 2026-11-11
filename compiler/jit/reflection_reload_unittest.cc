#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/development/development_session.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/jit/compiler.h"
#include "compiler/project/project_config.h"
#include "compiler/test_support/development_test_support.h"
#include "compiler/test_support/reflection_test_support.h"
#include "runtime/async/future.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

class ReflectionReloadTest : public ::testing::TestWithParam<NativeCodeTier> {
 protected:
  void SetUp() override {
    auto configuration = ParseProjectConfiguration("vela.json", R"({"reflection":[
      {"module":"main","type":"Box","members":[
        {"kind":"constructor"},{"kind":"field","name":"value"},{"kind":"method","name":"read"}]}
    ]})");
    ASSERT_TRUE(configuration);
    DevelopmentOptions options;
    options.gc_threshold = 1;
    options.compilation.tier = GetParam();
    options.reflection = configuration->reflection;
    auto session = DevelopmentSession::Create("main.vela", {}, std::move(options));
    ASSERT_TRUE(session);
    session_storage_ = std::move(*session);
  }
  DevelopmentResult Update(
      std::string source,
      std::optional<std::vector<frontend::ReflectionSelection>> reflection = std::nullopt,
      DevelopmentOperation operation = DevelopmentOperation::HotReload) {
    const auto revision = session_storage_->snapshot()->revision;
    return test::ApplyUpdate(*session_storage_,
                             {
                                 .base_revision = revision,
                                 .changes = {{.path = "main.vela", .text = std::move(source)}},
                                 .added_roots = {},
                                 .reflection = std::move(reflection),
                             },
                             revision == 0 ? DevelopmentOperation::Load : operation);
  }
  DevelopmentSession& session() { return *session_storage_; }
  Result<std::optional<TypeInfo>> FindType(std::string_view name) {
    auto reflection = Reflection::Bind(session().isolate().native_dispatch());
    if (!reflection) {
      return std::unexpected(std::move(reflection.error()));
    }
    return reflection->FindType("main.vela", name);
  }

 private:
  std::unique_ptr<DevelopmentSession> session_storage_;
};

TEST_P(ReflectionReloadTest, HostAndScriptHandlesSelectCurrentCodeWithoutRetainingVersions) {
  const std::string prefix = R"(
class Box { value: int = 10; read(): int { return this.value + )";
  const std::string suffix = R"(; } }
let object: Box? = null;
let method: MethodInfo? = null;
export func remember(): void {
  object = Box();
  method = Reflect.typeInfo<Box>().findMethod('read');
}
export func read(): int {
  const target = method;
  if (target == null) { return -1; }
  return target.invoke(object, CallArguments()) as int;
}
)";
  auto result = Update(prefix + "1" + suffix);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  ASSERT_TRUE(session().Invoke("main.vela", "remember"));
  const auto type = test::RequireReflectionValue(FindType("Box"));
  ASSERT_TRUE(type) << type.error();
  const auto method = test::RequireReflectionValue(type->FindMethod("read"));
  ASSERT_TRUE(method) << method.error();
  const auto ctor = test::RequireReflectionValue(type->GetConstructor());
  ASSERT_TRUE(ctor) << ctor.error();
  HandleScope scope(session().isolate());
  const auto receiver = ctor->Invoke(scope);
  ASSERT_TRUE(receiver);
  result = Update(prefix + "2" + suffix);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  const auto invoked = method->Invoke(scope, *receiver);
  ASSERT_TRUE(invoked) << invoked.error().message;
  EXPECT_EQ(internal::HandleAccess::Read(*invoked).AsInteger(), 12);
  const auto script = session().Invoke("main.vela", "read");
  ASSERT_TRUE(script);
  EXPECT_EQ(script->AsInteger(), 12);
  session().isolate().heap().Collect();
  EXPECT_EQ(session().code_resources().images, 1U);
}

TEST_P(ReflectionReloadTest, WithdrawalAndDeletionHaveDistinctErrorsAndOldNamesDoNotRevive) {
  auto result = Update("class Box { value: int = 10; read(): int { return this.value; } }");
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  const auto type = test::RequireReflectionValue(FindType("Box"));
  ASSERT_TRUE(type) << type.error();
  const auto field = test::RequireReflectionValue(type->FindField("value"));
  const auto method = test::RequireReflectionValue(type->FindMethod("read"));
  ASSERT_TRUE(field) << field.error();
  ASSERT_TRUE(method) << method.error();
  const auto old = field->id();
  result = Update("class Box { value: int = 10; read(): int { return this.value; } }",
                  std::vector<frontend::ReflectionSelection>{});
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  auto unavailable = field->Describe();
  ASSERT_FALSE(unavailable);
  EXPECT_EQ(unavailable.error().status, ExecutionStatus::ReflectionUnavailable);
  const auto unavailable_method = method->Describe();
  ASSERT_FALSE(unavailable_method);
  EXPECT_EQ(unavailable_method.error().status, ExecutionStatus::ReflectionUnavailable);
  EXPECT_EQ(result.changes.withdrawn_types.size(), 1U);
  EXPECT_EQ(result.changes.withdrawn_members.size(), 3U);
  EXPECT_TRUE(result.changes.deleted_members.empty());
  const auto described = type->Describe();
  ASSERT_TRUE(described);
  EXPECT_FALSE(described->is_registered);
  result = Update("class Box { }");
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  auto deleted = field->Describe();
  ASSERT_FALSE(deleted);
  EXPECT_EQ(deleted.error().status, ExecutionStatus::TargetDeleted);
  const auto deleted_method = method->Describe();
  ASSERT_FALSE(deleted_method);
  EXPECT_EQ(deleted_method.error().status, ExecutionStatus::TargetDeleted);
  auto config = ParseProjectConfiguration("vela.json", R"({"reflection":[
    {"module":"main","type":"Box","members":[{"kind":"field","name":"value"}]}
  ]})");
  ASSERT_TRUE(config);
  result = Update("class Box { value: int = 30; }", config->reflection);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  const auto replacement = test::RequireReflectionValue(type->FindField("value"));
  ASSERT_TRUE(replacement) << replacement.error();
  EXPECT_NE(replacement->id(), old);
  deleted = field->Describe();
  ASSERT_FALSE(deleted);
  EXPECT_EQ(deleted.error().status, ExecutionStatus::TargetDeleted);
}

TEST_P(ReflectionReloadTest, FieldHandlesFollowReorderingAndNewDefaultsInitializeOnce) {
  auto result = Update(R"(
let defaults = 0;
class Box {
  value: int = 10;
  other: string = 'old';
  read(): int { return this.value; }
}
export func count(): int { return defaults; }
)");
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  const auto type = test::RequireReflectionValue(FindType("Box"));
  ASSERT_TRUE(type) << type.error();
  const auto field = test::RequireReflectionValue(type->FindField("value"));
  const auto constructor = test::RequireReflectionValue(type->GetConstructor());
  ASSERT_TRUE(field) << field.error();
  ASSERT_TRUE(constructor) << constructor.error();
  const auto identity = field->id();
  HandleScope scope(session().isolate());
  const auto object = constructor->Invoke(scope);
  ASSERT_TRUE(object);
  auto configuration = ParseProjectConfiguration("vela.json", R"({"reflection":[
   {"module":"main","type":"Box","members":[{"kind":"constructor"},
    {"kind":"field","name":"value"},{"kind":"field","name":"added"},
    {"kind":"method","name":"read"}]}
  ]})");
  ASSERT_TRUE(configuration);
  result = Update(R"(
let defaults = 0;
func next(): int { defaults++; return 7; }
class Box {
  other: string = 'new';
  added: int = next();
  value: int = 20;
  read(): int { return this.value + this.added; }
}
export func count(): int { return defaults; }
)",
                  configuration->reflection);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  EXPECT_EQ(result.changes.registered_members.size(), 1U);
  const auto current = test::RequireReflectionValue(type->FindField("value"));
  const auto added = test::RequireReflectionValue(type->FindField("added"));
  ASSERT_TRUE(current) << current.error();
  ASSERT_TRUE(added) << added.error();
  EXPECT_EQ(current->id(), identity);
  auto value = field->GetValue(scope, *object);
  ASSERT_TRUE(value);
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 10);
  value = added->GetValue(scope, *object);
  ASSERT_TRUE(value) << value.error().message;
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 7);
  value = added->GetValue(scope, *object);
  ASSERT_TRUE(value);
  const auto count = session().Invoke("main.vela", "count");
  ASSERT_TRUE(count);
  EXPECT_EQ(count->AsInteger(), 1);
  const auto newer = constructor->Invoke(scope);
  ASSERT_TRUE(newer) << newer.error().message;
  value = field->GetValue(scope, *newer);
  ASSERT_TRUE(value);
  EXPECT_EQ(internal::HandleAccess::Read(*value).AsInteger(), 20);
}

TEST_P(ReflectionReloadTest, FailedCandidatesAndDeletedTargetsKeepScriptIdentitiesCoherent) {
  const std::string initial = R"(
class Box { value: int = 10; read(): int { return this.value; } }
let saved: FieldInfo? = null;
let identity: MemberId? = null;
export func remember(): void {
  saved = Reflect.typeInfo<Box>().findField('value');
  const current = saved as FieldInfo;
  identity = current.id;
}
export func status(): int {
  const current = saved as FieldInfo;
  if (current.id != identity) { return -1; }
  try { current.name; return 0; }
  catch (error: Error) { return error.code == 'TargetDeleted' ? 42 : -2; }
}
)";
  auto result = Update(initial);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  ASSERT_TRUE(session().Invoke("main.vela", "remember"));
  session().isolate().heap().Collect();
  const auto revision = session().snapshot()->revision;
  result = Update(initial + "func broken(:", std::vector<frontend::ReflectionSelection>{});
  EXPECT_EQ(result.status, DevelopmentStatus::SourceError);
  EXPECT_EQ(session().snapshot()->revision, revision);
  const auto unchanged = session().Invoke("main.vela", "status");
  ASSERT_TRUE(unchanged);
  EXPECT_EQ(unchanged->AsInteger(), 0);
  result = Update(R"(
class Box {}
let saved: FieldInfo? = null;
let identity: MemberId? = null;
export func status(): int {
  const current = saved as FieldInfo;
  if (current.id != identity) { return -1; }
  try { current.name; return 0; }
  catch (error: Error) { return error.code == 'TargetDeleted' ? 42 : -2; }
}
)",
                  std::vector<frontend::ReflectionSelection>{});
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  EXPECT_EQ(result.changes.deleted_members.size(), 2U);
  const auto deleted = session().Invoke("main.vela", "status");
  ASSERT_TRUE(deleted);
  EXPECT_EQ(deleted->AsInteger(), 42);
  session().isolate().heap().Collect();
  EXPECT_EQ(session().code_resources().images, 1U);
}

TEST_P(ReflectionReloadTest, ResolvesMethodIdentityBeforeReadingInvalidArgumentSnapshots) {
  const std::string prefix = R"(
let saved: MethodInfo? = null;
export func remember(): void { saved = Reflect.typeInfo<Box>().findMethod('read'); }
export func failure(): string {
  try {
    (saved as MethodInfo).invoke(null, CallArguments(items: [CallArgument(value: 0, name: '')]));
    return 'none';
  } catch (error: Error) { return error.code; }
}
)";
  const std::string owner =
      "class Box { value: int = 0; static read(value: int): int { return value; } }";
  auto result = Update(prefix + owner);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  ASSERT_TRUE(session().Invoke("main.vela", "remember"));
  result = Update(prefix + owner, std::vector<frontend::ReflectionSelection>{});
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  const auto withdrawn = session().Invoke("main.vela", "failure");
  ASSERT_TRUE(withdrawn);
  EXPECT_EQ(withdrawn->AsString()->value(), "ReflectionUnavailable");
  result = Update(prefix + "class Box {}", std::vector<frontend::ReflectionSelection>{});
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  const auto deleted = session().Invoke("main.vela", "failure");
  ASSERT_TRUE(deleted);
  EXPECT_EQ(deleted->AsString()->value(), "TargetDeleted");
}

TEST_P(ReflectionReloadTest, ExternalDescriptorsBlockFullReloadAndManagedDescriptorsDoNot) {
  const std::string source = R"(
class Box { value: int = 10; read(): int { return this.value; } }
let saved: TypeInfo? = null;
export func remember(): void { saved = Reflect.typeInfo<Box>(); }
func main(): void {}
)";
  auto result = Update(source);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  ASSERT_TRUE(session().Invoke("main.vela", "remember"));
  std::optional<TypeInfo> held;
  {
    const auto type = test::RequireReflectionValue(FindType("Box"));
    ASSERT_TRUE(type) << type.error();
    held = *type;
  }
  result = Update(source, std::nullopt, DevelopmentOperation::Reload);
  ASSERT_EQ(result.status, DevelopmentStatus::WaitingForRelease) << result.message;
  EXPECT_FALSE(result.blockers.empty());
  EXPECT_TRUE(held->Describe());
  held.reset();
  result = session().Pump();
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  EXPECT_EQ(session().generation(), 2U);
  const auto type = test::RequireReflectionValue(FindType("Box"));
  ASSERT_TRUE(type) << type.error();
  EXPECT_TRUE(type->Describe()->is_registered);
}

TEST_P(ReflectionReloadTest, StoppedContextsRejectMetadataAndExecutionOperations) {
  auto result = Update("class Box { value: int = 10; read(): int { return this.value; } }");
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  const auto reflection = Reflection::Bind(session().isolate().native_dispatch());
  ASSERT_TRUE(reflection);
  const auto type = test::RequireReflectionValue(reflection->FindType("main.vela", "Box"));
  ASSERT_TRUE(type) << type.error();
  const auto field = test::RequireReflectionValue(type->FindField("value"));
  ASSERT_TRUE(field) << field.error();
  const auto identity = field->id();
  session().isolate().native_control().RequestStop();
  const auto described = field->Describe();
  const auto types = reflection->GetTypes("main.vela");
  ASSERT_FALSE(described);
  ASSERT_FALSE(types);
  EXPECT_EQ(described.error().status, ExecutionStatus::ExecutionStopped);
  EXPECT_EQ(types.error().status, ExecutionStatus::ExecutionStopped);
  EXPECT_EQ(field->id(), identity);
}

TEST_P(ReflectionReloadTest, NotificationsUseStandardTypeDeclarationsAndStablePropertyIdentities) {
  auto configuration = ParseProjectConfiguration("vela.json", R"({"reflection":[
    {"module":"main","type":"Box","members":[{"kind":"getter","name":"amount"}]},
    {"module":"builtin:int","type":"int","members":[{"kind":"method","name":"parse"}]}
  ]})");
  ASSERT_TRUE(configuration);
  auto result = Update("class Box { value: int = 10; get amount(): int { return this.value; } }",
                       configuration->reflection);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  EXPECT_EQ(result.changes.registered_types.size(), 2U);
  const auto type = test::RequireReflectionValue(FindType("Box"));
  ASSERT_TRUE(type) << type.error();
  const auto property = test::RequireReflectionValue(type->FindProperty("amount"));
  ASSERT_TRUE(property) << property.error();
  const auto identity = property->id();
  EXPECT_EQ(property->CanRead(), true);
  EXPECT_EQ(property->CanWrite(), false);
  configuration = ParseProjectConfiguration("vela.json", R"({"reflection":[
    {"module":"main","type":"Box","members":[{"kind":"getter","name":"amount"},
      {"kind":"setter","name":"amount"}]}
  ]})");
  ASSERT_TRUE(configuration);
  result = Update(R"(
class Box {
  value: int = 10;
  get amount(): int { return this.value; }
  set amount(value: int): void { this.value = value; }
}
)",
                  configuration->reflection);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  EXPECT_EQ(result.changes.withdrawn_types.size(), 1U);
  EXPECT_EQ(result.changes.registered_members.size(), 1U);
  EXPECT_FALSE(result.changes.changed_members.empty());
  EXPECT_TRUE(result.changes.deleted_types.empty());
  const auto current = test::RequireReflectionValue(type->FindProperty("amount"));
  ASSERT_TRUE(current) << current.error();
  EXPECT_EQ(current->id(), identity);
  EXPECT_EQ(property->CanWrite(), true);
  const auto setter = test::RequireReflectionValue(property->GetSetter());
  ASSERT_TRUE(setter) << setter.error();
}

TEST_P(ReflectionReloadTest,
       StructuralDescriptorRootsRetainNestedNominalIdentitiesAcrossPublication) {
  auto result = Update(R"(
class Box { value: int = 10; read(): int { return this.value; } }
let saved: TypeInfo? = null;
let identity: TypeId? = null;
export func remember(): void {
  saved = Reflect.typeInfo<Map<string, (Box) => Future<Box[]>>>();
  identity = Reflect.typeInfo<Box>().id;
}
)");
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  ASSERT_TRUE(session().Invoke("main.vela", "remember"));
  result = Update(R"(
class Box {}
let saved: TypeInfo? = null;
let identity: TypeId? = null;
export func inspect(): int {
  const root = saved as TypeInfo;
  const function = root.typeArguments[1];
  const signature = function.signature as SignatureInfo;
  if (signature.parameters[0].type.id != identity) { return -1; }
  const array = signature.returnType.typeArguments[0];
  if (array.typeArguments[0].id != identity) { return -2; }
  return 42;
}
)",
                  std::vector<frontend::ReflectionSelection>{});
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  session().isolate().heap().Collect();
  EXPECT_EQ(session().code_resources().images, 1U);
  const auto inspected = session().Invoke("main.vela", "inspect");
  ASSERT_TRUE(inspected);
  EXPECT_EQ(inspected->AsInteger(), 42);
}

TEST_P(ReflectionReloadTest, AttributeSnapshotsKeepDataAndMethodsFollowTheCurrentPublication) {
  const std::string prefix = R"(
class Button extends Reflect { label: string = ''; }
class Box { )";
  const std::string suffix = R"( }
let saved: MethodAttributeMatch<Button>? = null;
export func remember(): void { saved = Reflect.findMethods<Button>()[0]; }
export func oldLabel(): string { return (saved as MethodAttributeMatch<Button>).attribute.label; }
export func currentLabel(): string { return Reflect.findMethods<Button>()[0].attribute.label; }
export func count(): int { return Reflect.findMethods<Button>().length; }
export func call(): int { return (saved as MethodAttributeMatch<Button>).method.invoke(null, CallArguments()) as int; }
)";
  auto result = Update(prefix + "[Button(label: 'old')] static read(): int { return 1; }" + suffix,
                       std::vector<frontend::ReflectionSelection>{});
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  ASSERT_TRUE(session().Invoke("main.vela", "remember"));
  result = Update(prefix + "[Button(label: 'new')] static read(): int { return 42; }" + suffix);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  EXPECT_EQ(result.changes.changed_members.size(), 1U);
  const auto old = session().Invoke("main.vela", "oldLabel");
  const auto current = session().Invoke("main.vela", "currentLabel");
  const auto call = session().Invoke("main.vela", "call");
  ASSERT_TRUE(old);
  ASSERT_TRUE(current);
  ASSERT_TRUE(call) << ExecutionStatusName(call.error());
  EXPECT_EQ(old->AsString()->value(), "old");
  EXPECT_EQ(current->AsString()->value(), "new");
  EXPECT_EQ(call->AsInteger(), 42);
  session().isolate().heap().Collect();
  EXPECT_EQ(session().code_resources().images, 1U);
  const auto revision = session().snapshot()->revision;
  result = Update(prefix + "[Button(label: text())] static read(): int { return 7; }" + suffix +
                  "func text(): string { return 'invalid'; }");
  EXPECT_NE(result.status, DevelopmentStatus::Accepted);
  EXPECT_EQ(session().snapshot()->revision, revision);
  EXPECT_EQ(session().Invoke("main.vela", "count")->AsInteger(), 1);
  result = Update(prefix + "static read(): int { return 0; }" + suffix);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  EXPECT_EQ(result.changes.withdrawn_members.size(), 1U);
  EXPECT_EQ(session().Invoke("main.vela", "count")->AsInteger(), 0);
  const auto withdrawn = session().Invoke("main.vela", "call");
  ASSERT_FALSE(withdrawn);
  EXPECT_EQ(withdrawn.error(), ExecutionStatus::ReflectionUnavailable);
  result = Update(prefix + suffix);
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  const auto deleted = session().Invoke("main.vela", "call");
  ASSERT_FALSE(deleted);
  EXPECT_EQ(deleted.error(), ExecutionStatus::TargetDeleted);
}

TEST_P(ReflectionReloadTest, SuspendedFramesDiscoverAttributesFromTheCurrentPublication) {
  const std::string prefix = "class Button extends Reflect { label: string = ''; } class Box { ";
  const std::string globals = R"( }
let gate = Completer<int>();
let pending: Future<void>? = null;
let label: string = '';
let value: int = 0;
let observedRevision: int = 0;
)";
  const std::string entries = R"(
export func start(): void { pending = observe(); }
export func release(): void { gate.complete(0); }
export func answer(): bool { return label == 'new' && value == 42 && observedRevision == 2; }
)";
  auto result =
      Update(prefix + "[Button(label: 'old')] static read(): int { return 1; }" + globals + R"(
async func observe(): Future<void> {
  await gate.future;
  const matches = Reflect.findMethods<Button>();
  label = matches[0].attribute.label;
  value = matches[0].method.invoke(null, CallArguments()) as int;
  observedRevision = Reflect.revision();
}
)" + entries,
             std::vector<frontend::ReflectionSelection>{});
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  ASSERT_TRUE(session().Invoke("main.vela", "start"));
  auto& isolate = session().isolate();
  auto& scheduler = isolate.scheduler();
  while (scheduler.PumpOne() == runtime::FuturePumpStep::Executed) {
  }
  ASSERT_EQ(scheduler.suspended_frame_count(), 1U);
  result = Update(prefix + "[Button(label: 'new')] static read(): int { return 42; }" + globals +
                  "async func observe(): Future<void> { await gate.future; label = 'wrong'; }" +
                  entries);
  // 旧帧新增的存留特化会在同一请求内补编译，再次进入发布安全点。
  if (result.status == DevelopmentStatus::WaitingForSafepoint) {
    result = session().Pump();
  }
  ASSERT_NO_FATAL_FAILURE(test::ExpectAccepted(result));
  ASSERT_TRUE(session().Invoke("main.vela", "release"));
  while (scheduler.PumpOne() == runtime::FuturePumpStep::Executed) {
  }
  const auto answer = session().Invoke("main.vela", "answer");
  ASSERT_TRUE(answer);
  EXPECT_TRUE(answer->AsBoolean());
  isolate.heap().Collect();
  EXPECT_EQ(session().code_resources().images, 1U);
}

INSTANTIATE_TEST_SUITE_P(ReflectionTiers,
                         ReflectionReloadTest,
                         ::testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::compiler::jit
