#include "compiler/project/project_config.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"
#include "gtest/gtest.h"

#include "base/process_exit.h"
#include "base/testing/directory.h"
#include "compiler/testing/settings.h"

namespace vela::compiler {
namespace {

TEST(ProjectConfigurationTest, ParsesExplicitReflectionWithoutImplicitMemberSelection) {
  const auto config = ParseProjectConfiguration("project/vela.json", R"({"reflection":[
    {"module":"src/counter","type":"Counter<int>","members":[
      {"kind":"field","name":"value"},
      {"kind":"method","name":"convert","typeArguments":["string?[]"]},
      {"kind":"getter","name":"count"},
      {"kind":"constructor"}
    ]},
    {"module":"sdk:test","type":"Matcher"}
  ]})");
  ASSERT_TRUE(config) << config.error().message;
  ASSERT_EQ(config->reflection.size(), 2U);
  const auto& first = config->reflection.front();
  EXPECT_EQ(first.module, "project/src/counter.vela");
  EXPECT_EQ(first.type.ToString(), "Counter<int>");
  ASSERT_EQ(first.members.size(), 4U);
  EXPECT_EQ(first.members.front().kind, frontend::ReflectionMemberKind::Field);
  EXPECT_EQ(first.members.at(1).type_arguments.front().ToString(), "string?[]");
  EXPECT_EQ(first.members.back().kind, frontend::ReflectionMemberKind::Constructor);
  EXPECT_TRUE(config->reflection.back().members.empty());
  EXPECT_EQ(first.source_path, "project/vela.json");
}

TEST(ProjectConfigurationTest, RejectsMalformedReflectionSelectionsAtTheirConfigurationLocation) {
  for (
      const auto* source : {
          R"({"reflection":{}})",
          R"({"reflection":[{}]})",
          R"({"reflection":[{"module":"app","type":"Box<"}]})",
          R"({"reflection":[{"module":"app","type":"int;"}]})",
          R"({"reflection":[{"module":"*.vela","type":"Counter"}]})",
          R"({"reflection":[{"module":"app","type":"Counter","members":true}]})",
          R"({"reflection":[{"module":"app","type":"Counter","members":[{}]}]})",
          R"({"reflection":[{"module":"app","type":"Counter","members":[{"kind":"all"}]}]})",
          R"({"reflection":[{"module":"app","type":"Counter","members":[{"kind":"field"}]}]})",
          R"({"reflection":[{"module":"app","type":"Counter","members":[{"kind":"constructor","name":"new"}]}]})",
          R"({"reflection":[{"module":"app","type":"Counter","members":[{"kind":"getter","name":"x","typeArguments":[]}]}]})",
          R"({"reflection":[{"module":"app","type":"Counter","members":[{"kind":"method","name":"x","typeArguments":["int;"]}]}]})",
      }) {
    const auto config = ParseProjectConfiguration("vela.json", source);
    ASSERT_FALSE(config) << source;
    EXPECT_TRUE(config.error().field.starts_with("reflection"));
    EXPECT_EQ(config.error().file, "vela.json");
    EXPECT_EQ(config.error().exit_code, kUsageExitCode);
  }
}

TEST(ProjectConfigurationTest, ReportsDeferredGenericConstraintErrorsAtTheConfigurationInput) {
  constexpr std::string_view kConfiguration = R"({
  "reflection": [
    {"module": "models", "type": "Limited<string>"}
  ]
})";
  const auto configuration = ParseProjectConfiguration("project/vela.json", kConfiguration);
  ASSERT_TRUE(configuration);
  const std::array sources{
      frontend::SourceInput{.path = "project/main.vela", .text = "func main(): void {}"},
      frontend::SourceInput{
          .path = "project/models.vela",
          .text = "class Limited<T extends int> { value: T; }",
      },
  };
  auto parsed = frontend::LoadSources(sources, sources.front().path, configuration->reflection);
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = frontend::TypeChecker().Check(parsed.program);
  ASSERT_FALSE(checked.Succeeded());
  for (const auto& diagnostic : checked.diagnostics) {
    EXPECT_EQ(diagnostic.source_path, "project/vela.json");
    EXPECT_EQ(diagnostic.location.line, 3U);
  }
}

TEST(ProjectConfigurationTest, LeavesMissingTestAndPathsForTheCommandBoundary) {
  const auto empty = ParseProjectConfiguration("project/vela.json", "{}");
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty->test);
  const auto configured = ParseProjectConfiguration("project/vela.json", R"({"test":{}})");
  ASSERT_TRUE(configured);
  const auto& test = configured->test;
  if (!test.has_value()) {
    ADD_FAILURE() << "missing test settings";
    return;
  }
  EXPECT_TRUE(test->paths.empty());
  EXPECT_EQ(test->filename, "*_test.vela");
  EXPECT_EQ(configured->root, "project");
}

TEST(ProjectConfigurationTest, ConfiguresDynamicConversionSeverityAndRejectsUnknownLevels) {
  const auto defaults = ParseProjectConfiguration("vela.json", "{}");
  ASSERT_TRUE(defaults);
  EXPECT_EQ(defaults->diagnostics.implicit_dynamic_conversion,
            frontend::DiagnosticSeverity::Warning);
  const auto strict = ParseProjectConfiguration(
      "vela.json", R"({"diagnostics":{"implicit_dynamic_conversion":"error"}})");
  ASSERT_TRUE(strict);
  EXPECT_EQ(strict->diagnostics.implicit_dynamic_conversion, frontend::DiagnosticSeverity::Error);
  for (const auto* source : {
           R"({"diagnostics":null})",
           R"({"diagnostics":{"implicit_dynamic_conversion":"off"}})",
           R"({"diagnostics":{"unknown":"error"}})",
       }) {
    EXPECT_FALSE(ParseProjectConfiguration("vela.json", source));
  }
}

TEST(ProjectConfigurationTest, PreservesTagSourceOrderAndExplicitFalseSkip) {
  const auto config = ParseProjectConfiguration("vela.json", R"({"test":{
    "paths":["tests/unit","tests/integration"], "filename":"*_spec.vela",
    "timeout":"2x", "suite_load_timeout":"1m", "concurrency":3, "reporter":"json",
    "include_tags":"unit && !network", "exclude_tags":"slow",
    "tags":{"unit":{"timeout":"10s","skip":true},"unit && !slow":{"skip":false}}
  }})");
  ASSERT_TRUE(config) << config.error().message;
  const auto& configured_settings = config->test;
  if (!configured_settings) {
    FAIL() << "missing test configuration";
  }
  const auto& settings = *configured_settings;
  EXPECT_EQ(settings.paths.size(), 2U);
  EXPECT_EQ(settings.filename, "*_spec.vela");
  EXPECT_EQ(settings.concurrency, 3U);
  EXPECT_EQ(settings.reporter, testing::Reporter::Json);
  ASSERT_EQ(settings.tags.size(), 2U);
  const auto& first = settings.tags.at(0);
  const auto& second = settings.tags.at(1);
  const std::vector<std::string> tags{"unit", "slow"};
  EXPECT_TRUE(first.selector.Matches(tags));
  EXPECT_FALSE(second.selector.Matches(tags));
  const auto& first_skip = first.skip;
  const auto& second_skip = second.skip;
  if (!first_skip.has_value()) {
    ADD_FAILURE() << "missing first skip setting";
    return;
  }
  EXPECT_TRUE(first_skip->enabled);
  if (!second_skip.has_value()) {
    ADD_FAILURE() << "missing second skip setting";
    return;
  }
  EXPECT_FALSE(second_skip->enabled);
}

TEST(ProjectConfigurationTest, RejectsInvalidJsonTypesUnknownFieldsAndMalformedSettings) {
  for (const auto* source : {
           "[]",
           "null",
           "{} {}",
           R"({/*comment*/"test":{}})",
           R"({"test":{},})",
           R"({"test":null})",
           R"({"other":{}})",
           R"({"test":{"unknown":1}})",
           R"({"test":{"paths":[]}})",
           R"({"test":{"paths":null}})",
           R"({"test":{"paths":[1]}})",
           R"({"test":{"paths":["*.vela"]}})",
           R"({"test":{"paths":["tests\\unit"]}})",
           R"({"test":{"filename":""}})",
           R"({"test":{"concurrency":0}})",
           R"({"test":{"concurrency":1.5}})",
           R"({"test":{"concurrency":"2"}})",
           R"({"test":{"concurrency":4294967296}})",
           R"({"test":{"timeout":"-1s"}})",
           R"({"test":{"suite_load_timeout":"2x"}})",
           R"({"test":{"reporter":"invalid"}})",
           R"({"test":{"include_tags":"unit &&"}})",
           R"({"test":{"tags":{"unit":{"bad":true}}}})",
           R"({"test":{"tags":{"unit":{"skip":null}}}})",
           R"({"test":{"tags":[]}})",
       }) {
    const auto config = ParseProjectConfiguration("vela.json", source);
    ASSERT_FALSE(config) << source;
    EXPECT_EQ(config.error().exit_code, kUsageExitCode) << source;
  }
}

TEST(ProjectConfigurationTest, LocatesEscapedDuplicateKeysAndNestedFieldErrors) {
  const auto duplicate = ParseProjectConfiguration(
      "vela.json", "{\n  \"test\": {\"concurrency\": 1, \"concurr\\u0065ncy\": 2}\n}");
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().field, "test.concurrency");
  EXPECT_EQ(duplicate.error().location.line, 2U);
  EXPECT_EQ(duplicate.error().location.column, 30U);
  EXPECT_EQ(duplicate.error().message, "duplicate JSON key");
  const auto typed =
      ParseProjectConfiguration("vela.json", "{\n  \"test\": {\"concurrency\": 0}\n}");
  ASSERT_FALSE(typed);
  EXPECT_EQ(typed.error().field, "test.concurrency");
  EXPECT_EQ(typed.error().location.line, 2U);
  EXPECT_EQ(typed.error().location.column, 27U);
}

class ProjectConfigurationFileTest : public test::DirectoryTest {
 protected:
  void SetUp() override {
    test::DirectoryTest::SetUp();
    ASSERT_FALSE(HasFatalFailure());
    std::error_code error;
    std::filesystem::create_directories(directory() / "project" / "nested", error);
    ASSERT_FALSE(error);
  }
};

TEST_F(ProjectConfigurationFileTest, FindsNearestConfigAndPublishesAnOwnedSnapshot) {
  Write("vela.json", R"({"test":{"concurrency":1}})");
  Write("project/vela.json", R"({"test":{"concurrency":2}})");
  const auto config = LoadProjectConfiguration(directory() / "project/nested");
  ASSERT_TRUE(config) << config.error().message;
  const auto& test = config->test;
  if (!test.has_value()) {
    ADD_FAILURE() << "missing test settings";
    return;
  }
  EXPECT_EQ(config->root, std::filesystem::canonical(directory() / "project"));
  EXPECT_EQ(test->concurrency, 2U);
  Write("project/vela.json", R"({"test":{"concurrency":3}})");
  EXPECT_EQ(test->concurrency, 2U);
  const auto next = LoadProjectConfiguration(directory() / "project/nested");
  ASSERT_TRUE(next);
  const auto& next_test = next->test;
  if (!next_test.has_value()) {
    ADD_FAILURE() << "missing test settings";
    return;
  }
  EXPECT_EQ(next_test->concurrency, 3U);
}

TEST_F(ProjectConfigurationFileTest, ExplicitConfigResolvesAgainstInvocationDirectory) {
  Write("vela.json", R"({"test":{"concurrency":1}})");
  Write("project/vela.json", R"({"test":{"concurrency":2}})");
  const auto config = LoadProjectConfiguration(directory() / "project/nested", "../../vela.json");
  ASSERT_TRUE(config);
  const auto& test = config->test;
  if (!test.has_value()) {
    ADD_FAILURE() << "missing test settings";
    return;
  }
  EXPECT_EQ(test->concurrency, 1U);
  const auto missing = LoadProjectConfiguration(directory(), "missing.json");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().exit_code, kIoErrorExitCode);
}

}  // namespace
}  // namespace vela::compiler
