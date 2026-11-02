#include <algorithm>
#include <array>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/test_operation.h"
#include "gtest/gtest.h"

#include "base/mock_operation.h"
#include "base/test_protocol.h"
#include "compiler/frontend/builtins/sdk_modules.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_file.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/typing/checker.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_program_state.h"
#include "runtime/testing/test_bridge.h"
#include "runtime/value/value.h"

namespace vela::compiler {
namespace {

struct ProtocolRecorder final : runtime::TestBridge {
  std::expected<void, std::string> Register(
      const runtime::TestRegistration& registration) override {
    registrations.push_back(registration.operation);
    return {};
  }
  void RegisterMockFailure(const runtime::NominalType& /*type*/, MockFailureKind kind) override {
    failures.push_back(kind);
  }
  std::expected<runtime::Value, std::string> Control(
      TestControl operation,
      std::span<const runtime::Value> /*arguments*/,
      std::vector<runtime::FutureTraceFrame> /*trace*/) override {
    controls.push_back(operation);
    return runtime::Value::Integer(1);
  }
  std::vector<runtime::TestOperation> registrations;
  std::vector<TestControl> controls;
  std::vector<MockFailureKind> failures;
};

TEST(SdkModulesTest, RegistrationAndControlNumbersReachTheirHostOperations) {
  runtime::Isolate isolate(1);
  const auto program = test::CompileCleanSource(R"(
import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('case', () => {});
  group('group', () => {});
  setUp(() => {});
  tearDown(() => {});
  setUpAll(() => {});
  tearDownAll(() => {});
  addTearDown(() => {});
  const callback = expectAsync0(() => {});
  callback();
  expectLater(Future.value(42), 42);
  const session = mock.MockSession();
  session.close();
})",
                                                isolate);
  ASSERT_TRUE(program) << program.error();
  ProtocolRecorder recorder;
  isolate.SetTestBridge(&recorder);
  const auto result = test::Execute(**program);
  isolate.SetTestBridge(nullptr);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  using Operation = runtime::TestOperation;
  EXPECT_EQ(recorder.registrations, (std::vector<Operation>{
                                        Operation::Test,
                                        Operation::EnterGroup,
                                        Operation::ExitGroup,
                                        Operation::SetUp,
                                        Operation::TearDown,
                                        Operation::SetUpAll,
                                        Operation::TearDownAll,
                                        Operation::AddTearDown,
                                    }));
  using Control = TestControl;
  EXPECT_EQ(recorder.controls, (std::vector<Control>{
                                   Control::RegisterCallback,
                                   Control::EnterCallback,
                                   Control::CheckCallback,
                                   Control::ExitCallback,
                                   Control::TrackExpectation,
                                   Control::CurrentTestScope,
                                   Control::RegisterFinalizer,
                                   Control::CurrentTestScope,
                                   Control::ReleaseFinalizer,
                               }));
}

TEST(SdkModulesTest, MockFailureNamesAndNumbersReachTheirHostCategories) {
  constexpr auto kinds = std::to_array<MockFailureKind>({
      MockFailureKind::Usage,
      MockFailureKind::MissingStub,
      MockFailureKind::StubExhausted,
      MockFailureKind::Verification,
      MockFailureKind::Reset,
      MockFailureKind::SessionClosed,
  });
  std::string source = "import 'sdk:mock' as mock; func main(): void {";
  for (const auto kind : kinds) {
    source += "mock.";
    source += MockFailureName(kind);
    source += "('contract');";
  }
  source += '}';
  runtime::Isolate isolate(1);
  const auto program = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(program) << program.error();
  ProtocolRecorder recorder;
  isolate.SetTestBridge(&recorder);
  const auto result = test::Execute(**program);
  isolate.SetTestBridge(nullptr);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(recorder.failures, (std::vector(kinds.begin(), kinds.end())));
}

TEST(SdkModulesTest, StructuredFailureFieldsMatchTheHostReader) {
  const auto source = frontend::SdkModuleSource(frontend::kTestModuleName);
  if (!source) {
    FAIL() << "missing SDK source";
  }
  const auto parsed = frontend::ParseSource(*source);
  ASSERT_TRUE(parsed.diagnostics.empty()) << frontend::DiagnosticsText(parsed.diagnostics);
  for (const auto& [name, fields] :
       std::to_array<std::pair<std::string_view, std::vector<std::string_view>>>({
           {
               test_fields::kFailureType,
               {test_fields::kMessage, test_fields::kDifference, mock_fields::kDetails},
           },
           {
               test_fields::kDifferenceType,
               {test_fields::kPath, test_fields::kActual, test_fields::kChildren},
           },
       })) {
    const auto owner = std::ranges::find(parsed.program.classes, name, &frontend::Class::name);
    ASSERT_NE(owner, parsed.program.classes.end()) << name;
    for (const auto field : fields) {
      EXPECT_NE(std::ranges::find(owner->fields, field, &frontend::Field::name),
                owner->fields.end())
          << name << '.' << field;
    }
  }
}

TEST(SdkModulesTest, SharesIdentityAcrossMemoryModulesAndNativeInstances) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import 'sdk:test' as t;
import 'support.vela';
func main(): int {
  const timeout = t.Timeout(milliseconds: 42);
  if (timeout.milliseconds != 42 || t.Timeout.factor(2.0).multiplier != 2.0) { return 0; }
  if (t.Timeout.none().mode != 2 || !valid()) { return 1; }
  try { t.fail('original message'); }
  catch (error: t.TestFailure) { if (error.message == 'original message') { return 42; } }
  return 2;
})",
      },
      {
          .path = "support.vela",
          .text = R"(
import 'sdk:test';
class Custom implements Matcher {
  match(actual: dynamic): MatchResult { return MatchResult(success: true, description: 'custom'); }
}
export func valid(): bool {
  const matcher: Matcher = Custom();
  return matcher.match(42).success;
})",
      },
  });
  auto loaded = frontend::LoadSources(sources, "main.vela");
  ASSERT_TRUE(loaded.diagnostics.empty());
  EXPECT_EQ(std::ranges::count(loaded.program.sources, "sdk:test", &frontend::SourceFile::path), 1);
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileParsed(std::move(loaded), isolate);
  EXPECT_TRUE(compiled.diagnostics.empty()) << frontend::DiagnosticsText(compiled.diagnostics);
  ASSERT_TRUE(compiled.Succeeded());
  const auto image =
      runtime::Heap::NativeProgram(*compiled.instance->state().front().AsObject()).image_owner;
  runtime::Isolate restored(1);
  const auto program = runtime::NativeInstance::Instantiate(restored, image);
  ASSERT_TRUE(program);
  const auto executed = test::Execute(**program);
  ASSERT_TRUE(executed.Succeeded()) << executed.message;
  EXPECT_EQ(executed.value.AsInteger(), 42);
}

TEST(SdkModulesTest, RejectsOverridesUnknownModulesAndUnrequestedDiskReads) {
  const auto overrides = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'sdk:test'; func main(): void {}"},
      {.path = "sdk:test", .text = "export class Fake {}"},
  });
  EXPECT_FALSE(frontend::CheckParsed(frontend::LoadSources(overrides, "main.vela")).Succeeded());
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'sdk:missing'; func main(): void {}"},
  });
  const auto unknown = frontend::LoadSources(sources, "main.vela");
  ASSERT_EQ(unknown.diagnostics.size(), 1U);
  EXPECT_EQ(unknown.diagnostics.front().message, "unknown SDK module 'sdk:missing'");
  const auto disk = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'compiler/sdk/test.vela'; func main(): void {}"},
  });
  EXPECT_FALSE(frontend::LoadSources(disk, "main.vela").diagnostics.empty());
}

}  // namespace
}  // namespace vela::compiler
