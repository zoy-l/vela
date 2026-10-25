#include "compiler/frontend/standard_environment.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/token.h"

namespace vela::frontend {
namespace {
constexpr auto kNames =
    std::to_array<std::string_view>({"Math", "Random", "performance", "Date", "Error"});
}

bool IsStandardName(std::string_view name) noexcept {
  return std::ranges::find(kNames, name) != kNames.end();
}

std::optional<std::string_view> StandardDeclarationSource(std::string_view name) noexcept {
  if (name == "Error") {
    return R"(class Error {
  public param message: string;
  public param code: string = 'Error';
}
)";
  }
  if (name == "performance") {
    return R"(class performance {
  private constructor() {}
  static external func now(): double = 'performance.now';
}
)";
  }
  if (name == "Date") {
    return R"(class Date {
  private constructor() {}
  static external func now(): int = 'Date.now';
}
)";
  }
  if (name == "Random") {
    return R"(class Random {
  private constructor() {}
  static external func seed(value: int): void = 'Random.seed';
  static external func nextInt(bound: int): int = 'Random.nextInt';
}
)";
  }
  if (name == "Math") {
    return R"(class Math {
  private constructor() {}
  static PI: double = 3.141592653589793;
  static E: double = 2.718281828459045;
  static external func random(): double = 'Math.random';
  static external func sqrt(value: double): double = 'Math.sqrt';
  static external func log(value: double): double = 'Math.log';
  static external func exp(value: double): double = 'Math.exp';
  static external func sin(value: double): double = 'Math.sin';
  static external func cos(value: double): double = 'Math.cos';
  static external func tan(value: double): double = 'Math.tan';
  static external func floor(value: double): double = 'Math.floor';
  static external func ceil(value: double): double = 'Math.ceil';
  static external func round(value: double): double = 'Math.round';
  static external func trunc(value: double): double = 'Math.trunc';
  static external func abs(value: double): double = 'Math.abs';
  static external func pow(left: double, right: double): double = 'Math.pow';
  static external func min(left: double, right: double): double = 'Math.min';
  static external func max(left: double, right: double): double = 'Math.max';
  static external func clamp(value: double, lower: double, upper: double): double = 'Math.clamp';
  static external func isFinite(value: double): bool = 'Math.isFinite';
  static external func isNaN(value: double): bool = 'Math.isNaN';
}
)";
  }
  return std::nullopt;
}

void AddStandardEnvironment(Program& program) {
  absl::flat_hash_set<std::string> used;
  for (const auto& source : program.sources) {
    // 大多数文件不使用标准对象，先用字符串搜索避开额外的 tokenize。
    bool candidate = false;
    for (const auto name : kNames) {
      if (source.text.contains(name)) {
        candidate = true;
        break;
      }
    }
    if (!candidate) {
      continue;
    }
    for (const auto& token : Lexer(source.text).Tokenize().tokens) {
      if (token.kind == TokenKind::Identifier && IsStandardName(token.lexeme)) {
        used.insert(token.lexeme);
      }
    }
  }
  for (const auto name : kNames) {
    if (!used.contains(name)) {
      continue;
    }
    const auto source = StandardDeclarationSource(name);
    assert(source);
    const auto source_id = static_cast<std::uint32_t>(program.sources.size());
    auto parsed = ParseSource(*source, source_id);
    assert(parsed.diagnostics.empty());
    AppendProgram(program, std::move(parsed.program));
    program.sources.push_back({
        .path = std::string(kStandardSourcePrefix) + std::string(name),
        .text = std::string(*source),
    });
  }
}

}  // namespace vela::frontend
