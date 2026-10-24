#include <iostream>
#include <string_view>

#include "compiler/frontend/checker.h"

namespace {

bool ExpectSuccess(std::string_view source) {
  const vela::frontend::CheckResult result = vela::frontend::CheckSource(source);
  if (result.Succeeded()) {
    return true;
  }
  for (const vela::frontend::Diagnostic& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.location.line << ':' << diagnostic.location.column << ": "
              << diagnostic.message << '\n';
  }
  return false;
}

bool ExpectFailure(std::string_view source) {
  return !vela::frontend::CheckSource(source).Succeeded();
}

}  // namespace

int main() {
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
  if (!ExpectSuccess(valid_source)) {
    return 1;
  }

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
  if (!ExpectFailure(invalid_source)) {
    return 1;
  }
  return 0;
}
