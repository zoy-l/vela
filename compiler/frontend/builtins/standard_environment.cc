#include "compiler/frontend/builtins/standard_environment.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "base/builtin.h"
#include "base/declaration.h"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/ast_clone.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {
namespace {
struct StandardDeclaration final {
  std::string_view name;
  std::string_view source;
};

constexpr auto kDeclarations = std::to_array<StandardDeclaration>({
    {
        .name = kMathTypeName,
        .source = R"(/// Mathematical constants and functions.
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
)",
    },
    {
        .name = kJsonTypeName,
        .source =
            R"(/// Strict in-memory JSON using native values, dynamic arrays, and ordered maps.
class Json {
  private constructor() {}
  /// Parses one complete JSON value. Invalid input throws JsonDecodeError with a byte offset.
  static external func parse(source: string): dynamic;
  /// Encodes compact JSON; objects use public toJson(). Cycles throw JsonEncodeError.
  static external func stringify(value: dynamic): string;
}
)",
    },
    {
        .name = kRandomTypeName,
        .source =
            R"(/// An isolate-local, reproducible pseudorandom generator, initially seeded with 0.
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
)",
    },
    {
        .name = kPerformanceTypeName,
        .source =
            R"(/// A monotonic clock for measuring elapsed time in milliseconds, including fractions.
/// Subtract two now() readings to measure a duration. Its origin is not Unix time.
class performance {
  private constructor() {}
  /// Returns the current monotonic clock reading in milliseconds as double.
  /// Consecutive readings may be equal. System clock adjustments do not move it backwards.
  /// Example: const start = performance.now();
  static external func now(): double = 'performance.now';
}
)",
    },
    {
        .name = kDateTypeName,
        .source =
            R"(/// Provides Unix wall-clock time, which may jump when the system clock is adjusted.
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
)",
    },
    {
        .name = kDatePartsTypeName,
        .source = R"(/// An immutable UTC calendar snapshot, created by Date.toUTC.
class DateParts {
  private yearValue: int = 0;
  private monthValue: int = 0;
  private dayValue: int = 0;
  private hourValue: int = 0;
  private minuteValue: int = 0;
  private secondValue: int = 0;
  private millisecondValue: int = 0;
  private constructor() {}
  get year(): int { return this.yearValue; }
  get month(): int { return this.monthValue; }
  get day(): int { return this.dayValue; }
  get hour(): int { return this.hourValue; }
  get minute(): int { return this.minuteValue; }
  get second(): int { return this.secondValue; }
  get millisecond(): int { return this.millisecondValue; }
}
)",
    },
    {
        .name = kByteArrayTypeName,
        .source =
            R"(/// Fixed-length, zero-initialized raw byte storage. Construct with ByteArray(length: int).
class ByteArray {
  private constructor() {}
  /// Validates and copies int[] elements in 0..255 into independently owned byte storage.
  static external func from(values: int[]): ByteArray;
}
)",
    },
    {
        .name = kByteDataTypeName,
        .source =
            R"(/// A typed byte view that retains its ByteArray owner. Construct with ByteData.view.
class ByteData {
  private constructor() {}
  /// Creates a shared view; offset defaults to zero, length to the remaining bytes.
  static external func view(bytes: ByteArray, offset: int, length: int): ByteData;
}
)",
    },
    {
        .name = kRegExpTypeName,
        .source =
            R"(/// PCRE2 Unicode regular expression with code-point coordinates; no shared cursor.
class RegExp {
  private constructor() {}
  /// Escapes literal text, including whitespace and extended-mode metacharacters.
  static external func escape(text: string): string;
}
)",
    },
    {
        .name = kRegExpMatchTypeName,
        .source =
            R"(/// Immutable match retaining its input and pattern. Created by RegExp searches.
class RegExpMatch {
  private constructor() {}
}
)",
    },
    {
        .name = kEndianTypeName,
        .source =
            R"(/// Byte order; multi-byte ByteData access defaults to big. host uses execution-platform order.
enum Endian { big, little, host }
)",
    },
    {
        .name = kIntTypeName,
        .source = R"(/// Signed 64-bit integers and explicit string parsing.
class int {
  private constructor() {}
  /// Parses the complete string as int64. radix is 2..36; omitted or null recognizes 0x.
  static external func parse(source: string, radix: int?): int;
  /// Returns null for invalid or overflowing text; an invalid radix still throws InvalidRadix.
  static external func tryParse(source: string, radix: int?): int?;
}
)",
    },
    {
        .name = kDoubleTypeName,
        .source = R"(/// IEEE 754 binary64 numbers and explicit string parsing.
class double {
  private constructor() {}
  /// Parses a complete decimal, NaN or Infinity, independently of locale.
  static external func parse(source: string): double;
  /// Returns null when the complete string is not a supported floating-point literal.
  static external func tryParse(source: string): double?;
}
)",
    },
    {
        .name = kErrorTypeName,
        .source =
            R"(/// A standard error that can be thrown and caught. Constructing an instance does not throw it.
/// message describes the failure; code is a program-readable identifier, defaulting to 'Error'.
class Error {
  /// A human-readable description of the failure.
  message: string;
  /// A program-readable error identifier, defaulting to 'Error'.
  code: string = 'Error';
}
)",
    },
    {
        .name = kTimeoutExceptionTypeName,
        .source = R"(/// The source Future did not complete before its timeout.
class TimeoutException {
  message: string = 'Future timed out';
  code: string = 'Timeout';
}
)",
    },
});

std::optional<Builtin> Intrinsic(std::string_view owner, const Function& declaration) {
  static const absl::flat_hash_map<std::pair<std::string_view, std::string_view>, Builtin>
      kIntrinsics = {
          {{kJsonTypeName, "parse"}, Builtin::JsonParse},
          {{kJsonTypeName, "stringify"}, Builtin::JsonStringify},
          {{kRegExpTypeName, "escape"}, Builtin::RegExpEscape},
          {{kByteArrayTypeName, "from"}, Builtin::ByteArrayFrom},
          {{kByteDataTypeName, "view"}, Builtin::ByteDataView},
          {{kIntTypeName, "parse"}, Builtin::IntParse},
          {{kIntTypeName, "tryParse"}, Builtin::IntTryParse},
          {{kDoubleTypeName, "parse"}, Builtin::DoubleParse},
          {{kDoubleTypeName, "tryParse"}, Builtin::DoubleTryParse},
          {{kDateTypeName, "parse"}, Builtin::DateParse},
          {{kDateTypeName, "toISOString"}, Builtin::DateToISOString},
          {{kDateTypeName, "fromUTC"}, Builtin::DateFromUTC},
          {{kDateTypeName, "toUTC"}, Builtin::DateToUTC},
      };
  const auto found = kIntrinsics.find({owner, declaration.name});
  return found == kIntrinsics.end() ? std::nullopt : std::optional(found->second);
}

std::shared_ptr<const ParsedSource> StandardSyntax(std::size_t index) {
  struct Entry final {
    std::once_flag once;
    std::shared_ptr<const ParsedSource> syntax;
  };
  static std::array<Entry, kDeclarations.size()> entries;
  auto& entry = entries.at(index);
  std::call_once(entry.once, [&] {
    auto syntax = std::make_shared<ParsedSource>(
        ParseSourceSnapshot(std::string(kDeclarations.at(index).source)));
    for (auto& declaration : syntax->program.classes) {
      for (auto& method : declaration.methods) {
        method.intrinsic = Intrinsic(declaration.name, method);
        if (method.intrinsic) {
          method.native_symbol.clear();
        }
      }
    }
    entry.syntax = std::move(syntax);
  });
  return entry.syntax;
}

}

std::vector<std::string> CollectStandardNames(std::span<const Token> tokens) {
  absl::flat_hash_set<std::string> used;
  constexpr auto kAsyncNames = std::to_array<std::string_view>(
      {kFutureTypeName, kCompleterTypeName, kTimerTypeName, kTimeoutExceptionTypeName});
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens.subspan(index).front();
    if (token.kind != TokenKind::Identifier) {
      continue;
    }
    if ((token.lexeme == kIntTypeName || token.lexeme == kDoubleTypeName) &&
        (index + 1 == tokens.size() || tokens.subspan(index + 1).front().kind != TokenKind::Dot)) {
      continue;
    }
    if (IsStandardName(token.lexeme)) {
      used.insert(token.lexeme);
      if (token.lexeme == kJsonTypeName) {
        used.insert(std::string(kErrorTypeName));
      }
    }
    if (std::ranges::find(kAsyncNames, token.lexeme) != kAsyncNames.end()) {
      used.insert(std::string(kErrorTypeName));
      used.insert(std::string(kTimeoutExceptionTypeName));
    }
  }
  return {used.begin(), used.end()};
}

bool IsStandardName(std::string_view name) noexcept {
  static const auto kNames = [] {
    absl::flat_hash_set<std::string_view> names;
    for (const auto& declaration : kDeclarations) {
      names.insert(declaration.name);
    }
    return names;
  }();
  return kNames.contains(name);
}

bool IsStandardTypeNamespace(const Program& program, const Class& declaration) {
  const auto& name = declaration.name;
  return (name == kIntTypeName || name == kDoubleTypeName || name == kByteArrayTypeName ||
          name == kByteDataTypeName || name == kRegExpTypeName || name == kRegExpMatchTypeName) &&
         declaration.location.source_id < program.sources.size() &&
         program.sources.at(declaration.location.source_id).path ==
             std::string(kStandardSourcePrefix) + name;
}

std::optional<std::string_view> StandardDeclarationSource(std::string_view name) noexcept {
  const auto declaration = std::ranges::find(kDeclarations, name, &StandardDeclaration::name);
  return declaration == kDeclarations.end() ? std::nullopt : std::optional(declaration->source);
}

Diagnostics AddStandardEnvironment(Program& program) {
  absl::flat_hash_set<std::string> used(program.standard_names.begin(),
                                        program.standard_names.end());
  for (const auto& alias : program.aliases) {
    if (alias.target.name == kIntTypeName || alias.target.name == kDoubleTypeName) {
      used.insert(alias.target.name);
    }
  }
  if (used.contains(kDateTypeName)) {
    used.insert(std::string(kDatePartsTypeName));
  }
  if (used.contains(kRegExpMatchTypeName)) {
    used.insert(std::string(kRegExpTypeName));
  }
  if (used.contains(kByteDataTypeName) || used.contains(kByteArrayTypeName)) {
    used.insert(std::string(kByteArrayTypeName));
    used.insert(std::string(kEndianTypeName));
  }
  for (std::size_t index = 0; index < kDeclarations.size(); ++index) {
    const auto& name = kDeclarations.at(index).name;
    if (!used.contains(name)) {
      continue;
    }
    const auto source_id = static_cast<std::uint32_t>(program.sources.size());
    const auto syntax = StandardSyntax(index);
    if (!syntax->diagnostics.empty()) {
      return syntax->diagnostics;
    }
    auto parsed = CloneProgram(syntax->program, {.source_id = source_id});
    const auto module_uri = std::string(kStandardSourcePrefix) + std::string(name);
    for (const auto& owner : parsed.classes) {
      parsed.origins.emplace(owner.name, DeclarationOrigin{
                                             .module_uri = module_uri,
                                             .name = owner.name,
                                             .kind = DeclarationKind::Class,
                                             .arguments = {},
                                             .type_parameters = {},
                                             .members = {},
                                             .generic_signature = {},
                                         });
    }
    for (const auto& enumeration : parsed.enums) {
      parsed.origins.emplace(enumeration.name, DeclarationOrigin{
                                                   .module_uri = module_uri,
                                                   .name = enumeration.name,
                                                   .kind = DeclarationKind::Enum,
                                                   .arguments = {},
                                                   .type_parameters = {},
                                                   .members = {},
                                                   .generic_signature = {},
                                               });
    }
    AppendProgram(program, std::move(parsed));
    program.sources.push_back({
        .path = module_uri,
        .text = syntax->source->text,
        .syntax = syntax->source,
    });
  }
  return {};
}

}  // namespace vela::frontend
