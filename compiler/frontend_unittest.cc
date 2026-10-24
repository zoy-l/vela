#include <string_view>

#include "gtest/gtest.h"

#include "compiler/frontend/checker.h"

namespace vela::frontend {
namespace {

TEST(FrontendTest, AcceptsNestedConstructorsAndContextualEnum) {
  constexpr std::string_view valid_source = R"(
enum Brightness {
  light,
  dark,
}

class Theme {
  Brightness brightness;
}

class App {
  Theme theme;
}

App build() {
  return App(theme: Theme(brightness: .light));
}
)";
  const CheckResult result = CheckSource(valid_source);
  for (const Diagnostic& diagnostic : result.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ':' << diagnostic.location.column << ": "
                  << diagnostic.message;
  }
  EXPECT_TRUE(result.Succeeded());
}

TEST(FrontendTest, RejectsUnknownEnumMember) {
  constexpr std::string_view invalid_source = R"(
enum Brightness {
  light,
}

class Theme {
  Brightness brightness;
}

Theme build() {
  return Theme(brightness: .missing);
}
)";
  const CheckResult result = CheckSource(invalid_source);
  EXPECT_FALSE(result.Succeeded());
  EXPECT_FALSE(result.diagnostics.empty());
}

}  // namespace
}  // namespace vela::frontend
