#include "compiler/frontend/standard_environment.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "base/builtin.h"

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/token.h"

namespace vela::frontend {
namespace {
constexpr auto kNames = std::to_array<std::string_view>({
    "Math",
    "Random",
    "performance",
    "Date",
    "DateParts",
    "int",
    "double",
    "Error",
    "TimeoutException",
});

std::optional<Builtin> Intrinsic(std::string_view owner, const Function& declaration) {
  const auto& member = declaration.name;
  if (owner == "int") {
    if (member == "parse") {
      return Builtin::IntParse;
    }
    if (member == "tryParse") {
      return Builtin::IntTryParse;
    }
  } else if (owner == "double") {
    if (member == "parse") {
      return Builtin::DoubleParse;
    }
    if (member == "tryParse") {
      return Builtin::DoubleTryParse;
    }
  } else if (owner == "Date") {
    if (member == "parse") {
      return Builtin::DateParse;
    }
    if (member == "toISOString") {
      return Builtin::DateToISOString;
    }
    if (member == "fromUTC") {
      return Builtin::DateFromUTC;
    }
    if (member == "toUTC") {
      return Builtin::DateToUTC;
    }
  }
  return std::nullopt;
}

bool HasStandardReferences(std::string_view text) {
  if (text.contains("Future") || text.contains("Completer") || text.contains("Timer")) {
    return true;
  }
  return std::ranges::any_of(kNames, [text](std::string_view name) {
    return text.contains(name) && ((name != "int" && name != "double") || text.contains('.'));
  });
}

void CollectSourceStandardNames(std::string_view text, absl::flat_hash_set<std::string>& used) {
  // 数字类型的普通标注不加载静态解析命名空间；只在成员访问或别名依赖时加入。
  if (!HasStandardReferences(text)) {
    return;
  }
  constexpr auto kAsyncNames =
      std::to_array<std::string_view>({"Future", "Completer", "Timer", "TimeoutException"});
  const auto tokens = Lexer(text).Tokenize().tokens;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens.at(index);
    if (token.kind != TokenKind::Identifier) {
      continue;
    }
    if ((token.lexeme == "int" || token.lexeme == "double") &&
        (index + 1 == tokens.size() || tokens.at(index + 1).kind != TokenKind::Dot)) {
      continue;
    }
    if (IsStandardName(token.lexeme)) {
      used.insert(token.lexeme);
    }
    if (std::ranges::find(kAsyncNames, token.lexeme) != kAsyncNames.end()) {
      used.insert("Error");
      used.insert("TimeoutException");
    }
  }
}

}

bool IsStandardName(std::string_view name) noexcept {
  return std::ranges::find(kNames, name) != kNames.end();
}

bool IsStandardScalarNamespace(const Program& program, const Class& declaration) {
  const auto& name = declaration.name;
  return (name == "int" || name == "double") &&
         declaration.location.source_id < program.sources.size() &&
         program.sources.at(declaration.location.source_id).path ==
             std::string(kStandardSourcePrefix) + name;
}

std::optional<std::string_view> StandardDeclarationSource(std::string_view name) noexcept {
  if (name == "int") {
    return R"(/// Signed 64-bit integers and explicit string parsing.
class int {
  private constructor() {}
  /// Parses the complete string as int64. radix is 2..36; omitted or null recognizes 0x.
  static external func parse(source: string, radix: int?): int;
  /// Returns null for invalid or overflowing text; an invalid radix still throws InvalidRadix.
  static external func tryParse(source: string, radix: int?): int?;
}
)";
  }
  if (name == "double") {
    return R"(/// IEEE 754 binary64 numbers and explicit string parsing.
class double {
  private constructor() {}
  /// Parses a complete decimal, NaN or Infinity, independently of locale.
  static external func parse(source: string): double;
  /// Returns null when the complete string is not a supported floating-point literal.
  static external func tryParse(source: string): double?;
}
)";
  }
  if (name == "DateParts") {
    return R"(/// An immutable UTC calendar snapshot, created by Date.toUTC.
class DateParts {
  private yearValue: int = 0;
  private monthValue: int = 0;
  private dayValue: int = 0;
  private hourValue: int = 0;
  private minuteValue: int = 0;
  private secondValue: int = 0;
  private millisecondValue: int = 0;
  private constructor() {}
  public get year(): int { return this.yearValue; }
  public get month(): int { return this.monthValue; }
  public get day(): int { return this.dayValue; }
  public get hour(): int { return this.hourValue; }
  public get minute(): int { return this.minuteValue; }
  public get second(): int { return this.secondValue; }
  public get millisecond(): int { return this.millisecondValue; }
}
)";
  }
  if (name == "TimeoutException") {
    return R"(/// The source Future did not complete before its timeout.
class TimeoutException {
  public param message: string = 'Future timed out';
  public param code: string = 'Timeout';
}
)";
  }
  if (name == "Error") {
    return R"(/// A standard error that can be thrown and caught. Constructing an instance does not throw it.
/// message describes the failure; code is a program-readable identifier, defaulting to 'Error'.
class Error {
  /// A human-readable description of the failure.
  public param message: string;
  /// A program-readable error identifier, defaulting to 'Error'.
  public param code: string = 'Error';
}
)";
  }
  if (name == "performance") {
    return R"(/// A monotonic clock for measuring elapsed time in milliseconds, including fractions.
/// Subtract two now() readings to measure a duration. Its origin is not Unix time.
class performance {
  private constructor() {}
  /// Returns the current monotonic clock reading in milliseconds as double.
  /// Consecutive readings may be equal. System clock adjustments do not move it backwards.
  /// Example: const start = performance.now();
  static external func now(): double = 'performance.now';
}
)";
  }
  if (name == "Date") {
    return R"(/// Provides Unix wall-clock time, which may jump when the system clock is adjusted.
class Date {
  private constructor() {}
  /// Returns integer milliseconds since 1970-01-01T00:00:00Z as int.
  /// Use performance.now() to measure elapsed time.
  static external func now(): int = 'Date.now';
  /// Parses strict ISO date-time text with an explicit offset; returns Unix milliseconds.
  static external func parse(source: string): int;
  /// Formats a supported Unix millisecond timestamp as YYYY-MM-DDTHH:mm:ss.SSSZ.
  static external func toISOString(timestamp: int): string;
  /// Validates UTC components and returns Unix milliseconds; omitted time fields are zero.
  static external func fromUTC(year: int, month: int, day: int, hour: int,
                              minute: int, second: int, millisecond: int): int;
  /// Returns a new immutable UTC calendar snapshot for a supported timestamp.
  static external func toUTC(timestamp: int): DateParts;
}
)";
  }
  if (name == "Random") {
    return R"(/// An isolate-local, reproducible pseudorandom generator, initially seeded with 0.
/// The same seed and call sequence produce the same results. Not cryptographically secure.
class Random {
  private constructor() {}
  /// Resets the random state using all 64 bits of the int seed value.
  /// Returns void. Affects subsequent nextInt() and Math.random() results.
  static external func seed(value: int): void = 'Random.seed';
  /// Returns a uniformly distributed random int in [0, bound).
  /// bound must be positive; otherwise throws a catchable Error.
  static external func nextInt(bound: int): int = 'Random.nextInt';
}
)";
  }
  if (name == "Math") {
    return R"(/// Mathematical constants and functions.
/// Numeric parameters use double without implicit int/double conversion.
/// All methods are static. Math cannot be constructed.
class Math {
  private constructor() {}
  /// Pi, approximately 3.141592653589793. A read-only double constant.
  static PI: double = 3.141592653589793;
  /// Euler's number e, approximately 2.718281828459045. A read-only double constant.
  static E: double = 2.718281828459045;
  /// Returns a pseudorandom double in [0.0, 1.0) using 53 random bits.
  /// Uses the current isolate's random state, resettable with Random.seed(value).
  static external func random(): double = 'Math.random';
  /// Returns the square root of value as double.
  /// value must be nonnegative; negative values throw a catchable Error.
  /// Example: Math.sqrt(9.0) returns 3.0.
  static external func sqrt(value: double): double = 'Math.sqrt';
  /// Returns the natural logarithm of value as double.
  /// value must be positive or an Error is thrown.
  static external func log(value: double): double = 'Math.log';
  /// Returns e raised to value as double. Overflow may produce Infinity.
  static external func exp(value: double): double = 'Math.exp';
  /// Returns the sine of value, measured in radians, as double.
  static external func sin(value: double): double = 'Math.sin';
  /// Returns the cosine of value, measured in radians, as double.
  static external func cos(value: double): double = 'Math.cos';
  /// Returns the tangent of value, measured in radians, as double.
  static external func tan(value: double): double = 'Math.tan';
  /// Rounds towards negative infinity and returns double. Example: Math.floor(-1.2) is -2.0.
  static external func floor(value: double): double = 'Math.floor';
  /// Rounds towards positive infinity and returns double. Example: Math.ceil(-1.2) is -1.0.
  static external func ceil(value: double): double = 'Math.ceil';
  /// Rounds to the nearest integer, with halfway cases away from zero. Returns double.
  /// Example: Math.round(-1.5) is -2.0.
  static external func round(value: double): double = 'Math.round';
  /// Truncates towards zero and returns double. Does not convert to int.
  static external func trunc(value: double): double = 'Math.trunc';
  /// Returns the absolute value of value as double.
  static external func abs(value: double): double = 'Math.abs';
  /// Returns left raised to the power right as double.
  static external func pow(left: double, right: double): double = 'Math.pow';
  /// Returns the smaller double. If exactly one argument is NaN, returns the other argument.
  static external func min(left: double, right: double): double = 'Math.min';
  /// Returns the larger double. If exactly one argument is NaN, returns the other argument.
  static external func max(left: double, right: double): double = 'Math.max';
  /// Restricts value to the closed interval [lower, upper] and returns double.
  /// Bounds must not be NaN and must satisfy lower <= upper; otherwise throws Error.
  static external func clamp(value: double, lower: double, upper: double): double = 'Math.clamp';
  /// Returns whether value is a finite double. NaN and positive or negative Infinity return false.
  static external func isFinite(value: double): bool = 'Math.isFinite';
  /// Returns whether value is NaN. Use this predicate instead of equality to detect NaN.
  static external func isNaN(value: double): bool = 'Math.isNaN';
}
)";
  }
  return std::nullopt;
}

void AddStandardEnvironment(Program& program) {
  absl::flat_hash_set<std::string> used;
  for (const auto& alias : program.aliases) {
    if (alias.target.name == "int" || alias.target.name == "double") {
      used.insert(alias.target.name);
    }
  }
  for (const auto& source : program.sources) {
    CollectSourceStandardNames(source.text, used);
  }
  if (used.contains("Date")) {
    used.insert("DateParts");
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
    for (auto& declaration : parsed.program.classes) {
      for (auto& method : declaration.methods) {
        method.intrinsic = Intrinsic(declaration.name, method);
        if (method.intrinsic) {
          method.native_symbol.clear();
        }
      }
    }
    AppendProgram(program, std::move(parsed.program));
    program.sources.push_back({
        .path = std::string(kStandardSourcePrefix) + std::string(name),
        .text = std::string(*source),
    });
  }
}

}  // namespace vela::frontend
